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

/* --- b60 freeze probe (design/FREEZE-AUDIT-b59.md §4/§9.1) -------------------
 * Bracket the un-deadlined synchronous provider traps with TickCount and report
 * any span past the warn threshold through the app's log sink. A healthy LAN
 * open/bind/connect/close is sub-tick, so this is silent in normal operation
 * and fires loudly — with the trap's name and the elapsed ticks — exactly when
 * one of them wedges the cooperative loop (the ~120 s = ~7200-tick event this
 * build exists to catch). CQ_TX_PROBE_TRACE additionally emits a breadcrumb
 * BEFORE each trap; DbgLog FlushVols every line, so that breadcrumb survives
 * even a trap that never returns (belt-and-suspenders — the observed freeze did
 * return, so the elapsed line alone localizes it). */
#ifndef CQ_TX_PROBE
#define CQ_TX_PROBE 1
#endif
#ifndef CQ_TX_PROBE_TRACE
#define CQ_TX_PROBE_TRACE 0
#endif
#define CQ_TX_PROBE_WARN_TICKS 6   /* ~100 ms; anything slower on a LAN is suspect */

static cq_tx_logfn g_log = NULL;

void cq_tx_set_log(cq_tx_logfn fn) { g_log = fn; }

#if CQ_TX_PROBE
static void probe_emit(const char *name, unsigned long elapsed)
{
    if (g_log && elapsed >= (unsigned long)CQ_TX_PROBE_WARN_TICKS) {
        char b[96];
        snprintf(b, sizeof(b), "ot-probe: %s +%lut (%lums)",
                 name, elapsed, elapsed * 1000UL / TICKS_PER_SEC);
        g_log(b);
    }
}
#if CQ_TX_PROBE_TRACE
static void probe_trace(const char *name)
{
    if (g_log) { char b[64]; snprintf(b, sizeof(b), "ot-probe: > %s", name); g_log(b); }
}
#else
#define probe_trace(name) ((void)0)
#endif
/* Time one synchronous provider call. `stmt` is the call (it may assign a
 * result); the elapsed TickCount span is reported if it exceeds the threshold. */
#define OT_PROBE(name, stmt) do {                                   \
        unsigned long _p0;                                         \
        probe_trace(name);                                         \
        _p0 = (unsigned long)TickCount();                          \
        stmt;                                                      \
        probe_emit(name, (unsigned long)TickCount() - _p0);        \
    } while (0)
#else
#define OT_PROBE(name, stmt) do { stmt; } while (0)
#endif

/* Endpoint teardown, probed: OTUnbind + OTCloseProvider are synchronous
 * provider calls no deadline governs, run on every transaction close/fail/
 * cancel/free — so every teardown site routes through here (§4). */
static void ot_teardown(EndpointRef ep)
{
    OT_PROBE("OTUnbind", OTUnbind(ep));
    OT_PROBE("OTCloseProvider", OTCloseProvider(ep));
}

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
    int            streaming;   /* endless response: drain-driven, no watchdog */
};

/* One-time OT startup (single-threaded cooperative app, so a static flag is safe). */
static int g_ot_up = 0;
static int ot_ensure_up(void)
{
    if (!g_ot_up) {
        OSStatus err;
        OT_PROBE("InitOpenTransport", err = InitOpenTransport());
        if (err != kOTNoError) return 0;
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
        ot_teardown(t->ep);
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

    OT_PROBE("OTOpenEndpoint",
             t->ep = OTOpenEndpoint(OTCreateConfiguration(kTCPName), 0, NULL, &err));
    if (err != kOTNoError || t->ep == kOTInvalidEndpointRef) {
        t->ep = kOTInvalidEndpointRef;
        fail(t, CQ_TX_ERR_CONNECT, "Could not open a TCP endpoint to");
        return;
    }
    OTSetSynchronous(t->ep);
    OTSetNonBlocking(t->ep);
    OTUseSyncIdleEvents(t->ep, false);

    OT_PROBE("OTBind", err = OTBind(t->ep, NULL, NULL));   /* dynamic local address */
    if (err != kOTNoError) { fail(t, CQ_TX_ERR_CONNECT, "Could not bind a socket to"); return; }

    if (!resolve_host(t->host, &ip)) { fail(t, CQ_TX_ERR_CONNECT, "Could not resolve"); return; }

    OTInitInetAddress(&sndAddr, (InetPort)t->port, ip);
    OTMemzero(&sndCall, sizeof(sndCall));
    sndCall.addr.buf = (UInt8 *)&sndAddr;
    sndCall.addr.len = sizeof(sndAddr);

    OT_PROBE("OTConnect", err = OTConnect(t->ep, &sndCall, NULL));
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
        OTResult r;
        /* Streaming backpressure: past the high-water mark, stop reading and
         * let the TCP window hold the server; cq_tx_drain() reopens the tap. */
        if (t->streaming && t->len >= CQ_TX_STREAM_HIWAT) return;
        r = OTRcv(t->ep, chunk, sizeof(chunk), &flags);
        if (r > 0) {
            if (append(t, chunk, (size_t)r) < 0) { fail(t, CQ_TX_ERR_STREAM, "Out of memory reading from"); return; }
            continue;
        }
        if (r == kOTNoDataErr) return;   /* nothing right now; poll again */
        if (r == kOTLookErr) {
            OTResult look = OTLook(t->ep);
            if (look == T_ORDREL) {              /* orderly release == EOF */
                OTRcvOrderlyDisconnect(t->ep);
                /* Answer with OUR half of the release before closing: without
                 * it the close is abortive (RST) and the server, waiting in
                 * FIN_WAIT for our FIN, logs a reset on EVERY successful
                 * transaction (Fio B). */
                OTSndOrderlyDisconnect(t->ep);
                ot_teardown(t->ep);
                t->ep = kOTInvalidEndpointRef;
                t->status = CQ_TX_DONE;
                t->st = ST_DONE;
                return;
            }
            if (look == T_DISCONNECT) {
                OTRcvDisconnect(t->ep, NULL);
                if (t->len > 0) {                /* got bytes then reset: keep them */
                    ot_teardown(t->ep); t->ep = kOTInvalidEndpointRef;
                    t->status = CQ_TX_DONE; t->st = ST_DONE;
                } else {
                    fail(t, CQ_TX_ERR_STREAM, "Read failed from");
                }
                return;
            }
            return;
        }
        /* any other negative result: if we already have bytes, deliver them
         * (best-effort orderly release first — connection state is unknown) */
        if (t->len > 0) {
            OTSndOrderlyDisconnect(t->ep);
            ot_teardown(t->ep); t->ep = kOTInvalidEndpointRef;
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
         * the watchdog. Both are TickCount deltas — the loop is the only clock.
         * A STREAMING transaction is only bounded until it reaches the receive
         * state: an endless response is the point, so from there the server's
         * close (T_ORDREL/T_DISCONNECT) is the sole way out. */
        if (t->st == ST_CONNECTING && (now - t->connect_tick) > (unsigned long)DEADLINE_TICKS)
            return fail(t, CQ_TX_ERR_TIMEOUT, "Timed out connecting to");
        if (!(t->streaming && t->st == ST_RECEIVING) &&
            (now - t->start_tick) > (unsigned long)WATCHDOG_TICKS)
            return fail(t, CQ_TX_ERR_TIMEOUT, "Timed out talking to");
    }
    return t->status;
}

cq_transport *cq_tx_stream_new(const char *host, int port, const char *selector)
{
    cq_transport *t = cq_tx_new(host, port, selector);
    if (t) t->streaming = 1;
    return t;
}

size_t cq_tx_drain(cq_transport *t, unsigned char *dst, size_t cap)
{
    size_t n;
    if (!t || !dst || cap == 0 || t->len == 0) return 0;
    n = t->len < cap ? t->len : cap;
    memcpy(dst, t->data, n);
    t->len -= n;
    if (t->len) memmove(t->data, t->data + n, t->len);
    return n;
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
        ot_teardown(t->ep);
        t->ep = kOTInvalidEndpointRef;
    }
}

void cq_tx_free(cq_transport *t)
{
    if (!t) return;
    if (t->ep != kOTInvalidEndpointRef) {
        ot_teardown(t->ep);
    }
    free(t->host);
    free(t->selector);
    free(t->req);
    free(t->data);
    free(t);
}

/* One-shot UDP datagram (network log mirror). Lazy singleton endpoint; only
 * fires once OT is ALREADY up (the transactions bring it up on first use) so
 * a boot-time log line can never drag InitOpenTransport in before the
 * Toolbox. Send errors (flow, unreachable) are silently dropped — it's a
 * log, not a transaction. */
static EndpointRef g_udp_ep = kOTInvalidEndpointRef;
static int         g_udp_state = 0;          /* 0 untried, 1 up, -1 dead */
static InetAddress g_udp_to;
static char        g_udp_host[64] = "";
static long        g_udp_ok = 0, g_udp_fail = 0, g_udp_lasterr = 0;

void cq_tx_udp_stats(long *ok, long *fail, long *lastErr)
{
    if (ok)      *ok      = g_udp_ok;
    if (fail)    *fail    = g_udp_fail;
    if (lastErr) *lastErr = g_udp_lasterr;
}

void cq_tx_udp(const char *host, int port, const void *data, size_t len)
{
    TUnitData ud;

    if (!host || !data || len == 0 || g_udp_state < 0) return;
    if (!g_ot_up) return;                    /* transactions haven't inited OT yet */

    if (g_udp_state == 0 || strcmp(g_udp_host, host) != 0) {
        OSStatus err;
        InetHost ip;
        if (g_udp_ep == kOTInvalidEndpointRef) {
            g_udp_ep = OTOpenEndpoint(OTCreateConfiguration(kUDPName), 0, NULL, &err);
            if (err != kOTNoError || g_udp_ep == kOTInvalidEndpointRef) {
                g_udp_ep = kOTInvalidEndpointRef;
                g_udp_state = -1;
                return;
            }
            OTSetSynchronous(g_udp_ep);
            OTSetNonBlocking(g_udp_ep);
            if (OTBind(g_udp_ep, NULL, NULL) != kOTNoError) {
                OTCloseProvider(g_udp_ep);
                g_udp_ep = kOTInvalidEndpointRef;
                g_udp_state = -1;
                return;
            }
        }
        if (!resolve_host(host, &ip)) { g_udp_state = -1; return; }
        OTInitInetAddress(&g_udp_to, (InetPort)port, ip);
        strncpy(g_udp_host, host, sizeof(g_udp_host) - 1);
        g_udp_host[sizeof(g_udp_host) - 1] = '\0';
        g_udp_state = 1;
    }

    OTMemzero(&ud, sizeof(ud));
    ud.addr.buf  = (UInt8 *)&g_udp_to;
    ud.addr.len  = sizeof(g_udp_to);
    ud.udata.buf = (UInt8 *)data;
    ud.udata.len = (ByteCount)len;
    {
        OTResult r = OTSndUData(g_udp_ep, &ud);
        if (r == kOTLookErr) {
            /* The OT UDP trap: one ICMP port-unreachable (listener not up
             * yet) queues a T_UDERR on the endpoint and EVERY later send
             * fails with kOTLookErr until it is cleared — the "exactly one
             * datagram ever arrives" symptom. Clear and retry once. */
            if (OTLook(g_udp_ep) == T_UDERR) OTRcvUDErr(g_udp_ep, NULL);
            r = OTSndUData(g_udp_ep, &ud);
        }
        if (r == kOTNoError) g_udp_ok++;
        else { g_udp_fail++; g_udp_lasterr = (long)r; }
    }
}

#endif /* CQ_OS9 */
