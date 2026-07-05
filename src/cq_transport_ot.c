/*
 * cq_transport_ot — Open Transport implementation of the transport seam, for
 * PowerPC / Mac OS 9. Compiled only in the app build (-DCQ_OS9); the host build
 * uses cq_transport_posix.c.
 *
 * ┌─ STATUS ────────────────────────────────────────────────────────────────┐
 * │ NOT YET COMPILED OR RUN. This is the on-VM piece: it needs the Retro68    │
 * │ toolchain (Universal Interfaces) to build and UTM/QEMU-PPC + the netatalk │
 * │ share to run. It is written against the OT API and mirrors the verified   │
 * │ POSIX impl's behavior, but MUST be validated on the VM (Fio 3's app is    │
 * │ the first thing that drives it) before it is trusted. Do not mark done.   │
 * └──────────────────────────────────────────────────────────────────────────┘
 *
 * Discipline (NOTES.md): the endpoint is opened SYNCHRONOUS + NON-BLOCKING and
 * advanced in bounded slices from cq_tx_poll(), which the app calls each pass of
 * the WaitNextEvent loop. NO blocking OT call (it would freeze the cooperative
 * machine); NO notifier doing work (we poll instead, so nothing runs at
 * deferred-task time). Deadlines are TickCount() deltas (60/sec): a LAN
 * transaction with no progress in ~2 s is dead, ~5 s is the outer watchdog.
 */
#if defined(CQ_OS9)

#include "cq_transport.h"

#include <OpenTransport.h>
#include <OpenTransportProviders.h>
#include <OpenTptInternet.h>
#include <Events.h>          /* TickCount */

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#define TICKS_PER_SEC     60
#define DEADLINE_TICKS    (CQ_TX_DEADLINE_SECS * TICKS_PER_SEC)   /* 120 */
#define WATCHDOG_TICKS    (CQ_TX_WATCHDOG_SECS * TICKS_PER_SEC)   /* 300 */

typedef enum {
    ST_INIT = 0,      /* open + bind the endpoint, resolve, issue connect */
    ST_CONNECTING,    /* waiting for T_CONNECT */
    ST_SENDING,       /* pushing "selector\r\n" */
    ST_RECEIVING,     /* draining to the orderly release (EOF) */
    ST_DONE,
    ST_FAILED
} ot_state;

struct cq_transport {
    char          *host;
    int            port;
    char          *selector;

    cq_tx_status   status;
    cq_tx_error    error;
    char           message[256];

    unsigned char *data;
    size_t         len;
    size_t         cap;

    EndpointRef    ep;
    ot_state       st;
    char          *req;        /* "selector\r\n" */
    size_t         reqlen;
    size_t         reqoff;
    unsigned long  start_tick;
    unsigned long  connect_tick;
    int            cancelled;
};

/* One-time OT startup (single-threaded cooperative app, so a static flag is safe). */
static int g_ot_up = 0;
static int ot_ensure_up(void)
{
    if (!g_ot_up) {
        if (InitOpenTransport() != kOTNoError) return 0;
        g_ot_up = 1;
    }
    return 1;
}

static char *dup_cstr(const char *s)
{
    size_t n = strlen(s ? s : "");
    char *o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s ? s : "", n + 1);
    return o;
}

cq_transport *cq_tx_new(const char *host, int port, const char *selector)
{
    cq_transport *t = (cq_transport *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->host     = dup_cstr(host);
    t->selector = dup_cstr(selector ? selector : "");
    t->port     = port;
    t->status   = CQ_TX_RUNNING;
    t->st       = ST_INIT;
    t->ep       = kOTInvalidEndpointRef;
    if (!t->host || !t->selector) { cq_tx_free(t); return NULL; }
    return t;
}

static cq_tx_status fail(cq_transport *t, cq_tx_error e, const char *what)
{
    t->status = CQ_TX_FAILED;
    t->st     = ST_FAILED;
    t->error  = e;
    snprintf(t->message, sizeof(t->message), "%s %s:%d.", what, t->host, t->port);
    if (t->ep != kOTInvalidEndpointRef) {
        OTSndOrderlyDisconnect(t->ep);   /* best-effort */
        OTUnbind(t->ep);
        OTCloseProvider(t->ep);
        t->ep = kOTInvalidEndpointRef;
    }
    return t->status;
}

static int append(cq_transport *t, const unsigned char *buf, size_t n)
{
    if (t->len + n > t->cap) {
        size_t ncap = t->cap ? t->cap : CQ_TX_READ_CHUNK;
        unsigned char *nd;
        while (ncap < t->len + n) ncap *= 2;
        nd = (unsigned char *)realloc(t->data, ncap);
        if (!nd) return -1;
        t->data = nd;
        t->cap  = ncap;
    }
    memcpy(t->data + t->len, buf, n);
    t->len += n;
    return 0;
}

/* Parse a dotted-quad "a.b.c.d" into an InetHost; returns 0 on failure. */
static int parse_dotted_quad(const char *s, InetHost *out)
{
    unsigned long parts[4];
    int i = 0;
    const char *p = s;
    for (i = 0; i < 4; i++) {
        unsigned long v = 0;
        int digits = 0;
        while (*p >= '0' && *p <= '9') { v = v * 10 + (unsigned long)(*p - '0'); p++; digits++; }
        if (!digits || v > 255) return 0;
        parts[i] = v;
        if (i < 3) { if (*p != '.') return 0; p++; }
    }
    if (*p != '\0') return 0;
    *out = (InetHost)((parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3]);
    return 1;
}

/* Resolve host -> InetHost. Numeric dotted-quad (the LAN spot host) resolves
 * with no DNS; a real hostname (the gopher browser, later) goes through OT's
 * resolver via OTInetStringToAddress. Returns 0 on failure. */
static int resolve_host(const char *host, InetHost *out)
{
    InetSvcRef isv;
    OSStatus err;
    struct InetHostInfo info;

    if (parse_dotted_quad(host, out)) return 1;

    isv = OTOpenInternetServices(kDefaultInternetServicesPath, 0, &err);
    if (err != kOTNoError || isv == NULL) return 0;
    err = OTInetStringToAddress(isv, (char *)host, &info);   /* blocking DNS */
    OTCloseProvider(isv);
    if (err != kOTNoError || info.addrs[0] == 0) return 0;
    *out = info.addrs[0];
    return 1;
}

static void open_and_connect(cq_transport *t)
{
    OSStatus     err;
    InetHost     ip;
    InetAddress  sndAddr;
    TCall        sndCall;

    if (!ot_ensure_up()) { fail(t, CQ_TX_ERR_CONNECT, "Open Transport unavailable for"); return; }

    t->ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err);
    if (err != kOTNoError || t->ep == kOTInvalidEndpointRef) {
        t->ep = kOTInvalidEndpointRef;
        fail(t, CQ_TX_ERR_CONNECT, "Could not open a TCP endpoint to");
        return;
    }
    OTSetSynchronous(t->ep);
    OTSetNonBlocking(t->ep);
    OTUseSyncIdleEvents(t->ep, false);

    err = OTBind(t->ep, NULL, NULL);       /* dynamic local address */
    if (err != kOTNoError) { fail(t, CQ_TX_ERR_CONNECT, "Could not bind a socket to"); return; }

    if (!resolve_host(t->host, &ip)) { fail(t, CQ_TX_ERR_CONNECT, "Could not resolve"); return; }

    OTInitInetAddress(&sndAddr, (InetPort)t->port, ip);
    OTMemzero(&sndCall, sizeof(sndCall));
    sndCall.addr.buf = (UInt8 *)&sndAddr;
    sndCall.addr.len = sizeof(sndAddr);

    err = OTConnect(t->ep, &sndCall, NULL);
    if (err == kOTNoError) { t->st = ST_SENDING; return; }        /* immediate */
    if (err == kOTNoDataErr) { t->st = ST_CONNECTING; return; }   /* in progress */
    fail(t, CQ_TX_ERR_CONNECT, "Could not connect to");
}

static void pump_connect(cq_transport *t)
{
    OTResult look = OTLook(t->ep);
    if (look == T_CONNECT) {
        if (OTRcvConnect(t->ep, NULL) == kOTNoError) t->st = ST_SENDING;
        else fail(t, CQ_TX_ERR_CONNECT, "Could not connect to");
    } else if (look == T_DISCONNECT) {
        OTRcvDisconnect(t->ep, NULL);
        fail(t, CQ_TX_ERR_CONNECT, "Connection refused by");
    }
    /* else: still connecting; the deadline check in cq_tx_poll bounds it */
}

static void pump_send(cq_transport *t)
{
    if (!t->req) {
        size_t sl = strlen(t->selector);
        t->reqlen = sl + 2;
        t->req = (char *)malloc(t->reqlen);
        if (!t->req) { fail(t, CQ_TX_ERR_STREAM, "Out of memory for"); return; }
        memcpy(t->req, t->selector, sl);
        t->req[sl]     = '\r';
        t->req[sl + 1] = '\n';
        t->reqoff = 0;
    }
    while (t->reqoff < t->reqlen) {
        OTResult r = OTSnd(t->ep, t->req + t->reqoff, t->reqlen - t->reqoff, 0);
        if (r >= 0) { t->reqoff += (size_t)r; continue; }
        if (r == kOTFlowErr) return;   /* buffer full; try again next poll */
        fail(t, CQ_TX_ERR_STREAM, "Write failed to");
        return;
    }
    t->st = ST_RECEIVING;
}

static void pump_recv(cq_transport *t)
{
    unsigned char chunk[CQ_TX_READ_CHUNK];
    OTFlags flags;
    for (;;) {
        OTResult r = OTRcv(t->ep, chunk, sizeof(chunk), &flags);
        if (r > 0) {
            if (append(t, chunk, (size_t)r) < 0) { fail(t, CQ_TX_ERR_STREAM, "Out of memory reading from"); return; }
            continue;
        }
        if (r == kOTNoDataErr) return;   /* nothing right now; poll again */
        if (r == kOTLookErr) {
            OTResult look = OTLook(t->ep);
            if (look == T_ORDREL) {              /* orderly release == EOF */
                OTRcvOrderlyDisconnect(t->ep);
                OTUnbind(t->ep);
                OTCloseProvider(t->ep);
                t->ep = kOTInvalidEndpointRef;
                t->status = CQ_TX_DONE;
                t->st = ST_DONE;
                return;
            }
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(t->ep, NULL);
                if (t->len > 0) {                /* got bytes then reset: keep them */
                    OTUnbind(t->ep); OTCloseProvider(t->ep); t->ep = kOTInvalidEndpointRef;
                    t->status = CQ_TX_DONE; t->st = ST_DONE;
                } else {
                    fail(t, CQ_TX_ERR_STREAM, "Read failed from");
                }
                return;
            }
            return;
        }
        /* any other negative result: if we already have bytes, deliver them */
        if (t->len > 0) {
            OTUnbind(t->ep); OTCloseProvider(t->ep); t->ep = kOTInvalidEndpointRef;
            t->status = CQ_TX_DONE; t->st = ST_DONE;
        } else {
            fail(t, CQ_TX_ERR_STREAM, "Read failed from");
        }
        return;
    }
}

void cq_tx_start(cq_transport *t)
{
    if (!t) return;
    t->start_tick = (unsigned long)TickCount();
    t->connect_tick = t->start_tick;
}

cq_tx_status cq_tx_poll(cq_transport *t)
{
    unsigned long now;
    if (!t) return CQ_TX_FAILED;
    if (t->cancelled || t->status != CQ_TX_RUNNING) return t->status;

    now = (unsigned long)TickCount();

    switch (t->st) {
        case ST_INIT:       open_and_connect(t); break;
        case ST_CONNECTING: pump_connect(t);     break;
        case ST_SENDING:    pump_send(t);        break;
        case ST_RECEIVING:  pump_recv(t);        break;
        default: break;
    }

    if (t->status == CQ_TX_RUNNING) {
        /* connect must land within the connect deadline; the whole thing within
         * the watchdog. Both are TickCount deltas — the loop is the only clock. */
        if (t->st == ST_CONNECTING && (now - t->connect_tick) > (unsigned long)DEADLINE_TICKS)
            return fail(t, CQ_TX_ERR_TIMEOUT, "Timed out connecting to");
        if ((now - t->start_tick) > (unsigned long)WATCHDOG_TICKS)
            return fail(t, CQ_TX_ERR_TIMEOUT, "Timed out talking to");
    }
    return t->status;
}

const unsigned char *cq_tx_data(const cq_transport *t, size_t *len)
{
    if (len) *len = t ? t->len : 0;
    return t ? t->data : NULL;
}

cq_tx_error cq_tx_error_code(const cq_transport *t) { return t ? t->error : CQ_TX_ERR_CONNECT; }
const char *cq_tx_error_message(const cq_transport *t) { return t ? t->message : ""; }

void cq_tx_cancel(cq_transport *t)
{
    if (!t) return;
    t->cancelled = 1;
    if (t->ep != kOTInvalidEndpointRef) {
        OTSndOrderlyDisconnect(t->ep);   /* we stop listening; a sent selector still ran */
        OTUnbind(t->ep);
        OTCloseProvider(t->ep);
        t->ep = kOTInvalidEndpointRef;
    }
}

void cq_tx_free(cq_transport *t)
{
    if (!t) return;
    if (t->ep != kOTInvalidEndpointRef) {
        OTUnbind(t->ep);
        OTCloseProvider(t->ep);
    }
    free(t->host);
    free(t->selector);
    free(t->req);
    free(t->data);
    free(t);
}

#endif /* CQ_OS9 */
