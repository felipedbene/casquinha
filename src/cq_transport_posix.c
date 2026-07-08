/*
 * cq_transport_posix — BSD-socket implementation of the transport seam.
 *
 * Used by host builds and the offline test (a localhost loopback). It mirrors
 * DeGelato's DGGopherClient path exactly — the one "nc uses": getaddrinfo with
 * AF_UNSPEC, a non-blocking connect bounded by select(), then blocking I/O with
 * SO_RCVTIMEO/SO_SNDTIMEO, and a recv() loop to EOF. The whole transaction runs
 * on the first cq_tx_poll() (a blocking host impl satisfies the poll interface).
 * The Open Transport implementation (cq_transport_ot.c) is the incremental one.
 */
#if !defined(CQ_OS9)

/* Expose strdup / getaddrinfo / BSD sockets under -std=c99 on Darwin and glibc. */
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE 1
#endif
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "cq_transport.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>

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

    int            started;
    int            cancelled;
    int            streaming;   /* endless response: drain-driven, no watchdog */
    int            fd;          /* streaming: live socket between polls */
};

cq_transport *cq_tx_new(const char *host, int port, const char *selector)
{
    cq_transport *t = (cq_transport *)calloc(1, sizeof(*t));
    if (!t) return NULL;
    t->host     = host ? strdup(host) : strdup("");
    t->selector = strdup(selector ? selector : "");
    t->port     = port;
    t->status   = CQ_TX_RUNNING;
    t->error    = CQ_TX_ERR_NONE;
    t->fd       = -1;
    if (!t->host || !t->selector) { cq_tx_free(t); return NULL; }
    return t;
}

cq_transport *cq_tx_stream_new(const char *host, int port, const char *selector)
{
    cq_transport *t = cq_tx_new(host, port, selector);
    if (t) t->streaming = 1;
    return t;
}

static cq_tx_status fail(cq_transport *t, cq_tx_error e, const char *fmt)
{
    t->status = CQ_TX_FAILED;
    t->error  = e;
    snprintf(t->message, sizeof(t->message), "%s %s:%d.", fmt, t->host, t->port);
    return t->status;
}

static int append(cq_transport *t, const unsigned char *buf, size_t n)
{
    if (t->len + n > t->cap) {
        size_t ncap = t->cap ? t->cap : CQ_TX_READ_CHUNK;
        while (ncap < t->len + n) ncap *= 2;
        {
            unsigned char *nd = (unsigned char *)realloc(t->data, ncap);
            if (!nd) return -1;
            t->data = nd;
            t->cap  = ncap;
        }
    }
    memcpy(t->data + t->len, buf, n);
    t->len += n;
    return 0;
}

/* Non-blocking connect bounded by select(); returns fd, or -1 with *err set. */
static int connect_deadline(struct addrinfo *ai, cq_tx_error *err)
{
    int fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
    int flags, r;
    fd_set wset;
    struct timeval tv;

    if (fd < 0) { *err = CQ_TX_ERR_CONNECT; return -1; }

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    r = connect(fd, ai->ai_addr, ai->ai_addrlen);
    if (r == 0) { fcntl(fd, F_SETFL, flags); return fd; }   /* immediate */
    if (errno != EINPROGRESS) { close(fd); *err = CQ_TX_ERR_CONNECT; return -1; }

    FD_ZERO(&wset);
    FD_SET(fd, &wset);
    tv.tv_sec  = CQ_TX_DEADLINE_SECS;
    tv.tv_usec = 0;
    r = select(fd + 1, NULL, &wset, NULL, &tv);
    if (r == 0) { close(fd); *err = CQ_TX_ERR_TIMEOUT; return -1; }
    if (r < 0)  { close(fd); *err = CQ_TX_ERR_CONNECT; return -1; }

    {   /* confirm the connect actually succeeded */
        int soerr = 0;
        socklen_t sl = sizeof(soerr);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &sl) < 0 || soerr != 0) {
            close(fd); *err = CQ_TX_ERR_CONNECT; return -1;
        }
    }
    fcntl(fd, F_SETFL, flags);   /* back to blocking */
    return fd;
}

/* Resolve + connect + write "selector\r\n". Returns the connected fd, or -1
 * after setting the failure on t. */
static int connect_and_send(cq_transport *t)
{
    struct addrinfo hints, *res = NULL, *ai;
    char portstr[16];
    int fd = -1, gai;
    cq_tx_error err = CQ_TX_ERR_CONNECT;
    struct timeval tv;
    char *line;
    size_t linelen, off;

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_UNSPEC;     /* the exact path nc uses */
    hints.ai_socktype = SOCK_STREAM;
    snprintf(portstr, sizeof(portstr), "%d", t->port);

    gai = getaddrinfo(t->host, portstr, &hints, &res);
    if (gai != 0 || !res) { fail(t, CQ_TX_ERR_CONNECT, "Could not resolve"); return -1; }

    for (ai = res; ai; ai = ai->ai_next) {
        fd = connect_deadline(ai, &err);
        if (fd >= 0) break;
    }
    freeaddrinfo(res);
    if (fd < 0) {
        fail(t, err, err == CQ_TX_ERR_TIMEOUT ? "Timed out connecting to"
                                              : "Could not connect to");
        return -1;
    }

    /* blocking read/write deadlines */
    tv.tv_sec = CQ_TX_DEADLINE_SECS; tv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    /* write "selector\r\n" fully */
    linelen = strlen(t->selector) + 2;
    line = (char *)malloc(linelen + 1);
    if (!line) { close(fd); fail(t, CQ_TX_ERR_STREAM, "Out of memory for"); return -1; }
    memcpy(line, t->selector, strlen(t->selector));
    line[linelen - 2] = '\r';
    line[linelen - 1] = '\n';
    line[linelen]     = '\0';

    off = 0;
    while (off < linelen) {
        ssize_t w = send(fd, line + off, linelen - off, 0);
        if (w <= 0) { free(line); close(fd); fail(t, CQ_TX_ERR_STREAM, "Write failed to"); return -1; }
        off += (size_t)w;
    }
    free(line);
    return fd;
}

static void run(cq_transport *t)
{
    int fd = connect_and_send(t);
    if (fd < 0) return;

    /* read to EOF */
    for (;;) {
        unsigned char chunk[CQ_TX_READ_CHUNK];
        ssize_t n = recv(fd, chunk, sizeof(chunk), 0);
        if (n == 0) break;                       /* clean EOF */
        if (n < 0) {
            /* deliver whatever we got; only a zero-length read error is fatal */
            if (t->len == 0) { close(fd); fail(t, CQ_TX_ERR_STREAM, "Read failed from"); return; }
            break;
        }
        if (append(t, chunk, (size_t)n) < 0) {
            close(fd); fail(t, CQ_TX_ERR_STREAM, "Out of memory reading from"); return;
        }
    }
    close(fd);
    t->status = CQ_TX_DONE;
}

void cq_tx_start(cq_transport *t)
{
    if (t) t->started = 1;
}

/* One streaming poll slice: non-blocking reads until EAGAIN / high water. */
static void pump_stream(cq_transport *t)
{
    if (t->fd < 0) {                 /* first poll: connect + send, go nonblocking */
        t->fd = connect_and_send(t);
        if (t->fd < 0) return;
        fcntl(t->fd, F_SETFL, fcntl(t->fd, F_GETFL, 0) | O_NONBLOCK);
    }
    for (;;) {
        unsigned char chunk[CQ_TX_READ_CHUNK];
        ssize_t n;
        if (t->len >= CQ_TX_STREAM_HIWAT) return;   /* backpressure: stop reading */
        n = recv(t->fd, chunk, sizeof(chunk), 0);
        if (n > 0) {
            if (append(t, chunk, (size_t)n) < 0) {
                close(t->fd); t->fd = -1;
                fail(t, CQ_TX_ERR_STREAM, "Out of memory reading from");
            }
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        close(t->fd); t->fd = -1;
        if (n == 0) t->status = CQ_TX_DONE;          /* server closed the mount */
        else        fail(t, CQ_TX_ERR_STREAM, "Read failed from");
        return;
    }
}

cq_tx_status cq_tx_poll(cq_transport *t)
{
    if (!t) return CQ_TX_FAILED;
    if (t->cancelled) return t->status;
    if (t->streaming) {
        if (t->started && t->status == CQ_TX_RUNNING) pump_stream(t);
        return t->status;
    }
    if (t->started && t->status == CQ_TX_RUNNING) {
        t->started = 0;   /* run once */
        run(t);
    }
    return t->status;
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
    if (t->fd >= 0) { close(t->fd); t->fd = -1; }
}

void cq_tx_free(cq_transport *t)
{
    if (!t) return;
    if (t->fd >= 0) close(t->fd);
    free(t->host);
    free(t->selector);
    free(t->data);
    free(t);
}

/* One-shot UDP datagram (network log mirror) — best-effort, errors dropped. */
static long g_udp_ok = 0, g_udp_fail = 0, g_udp_lasterr = 0;

void cq_tx_udp_stats(long *ok, long *fail, long *lastErr)
{
    if (ok)      *ok      = g_udp_ok;
    if (fail)    *fail    = g_udp_fail;
    if (lastErr) *lastErr = g_udp_lasterr;
}

/* Freeze probe sink (b60): accepted for contract parity. The POSIX impl
 * completes a transaction on the first poll with none of the OT provider
 * traps, so it never has a span to report — the sink is stored but unused. */
static cq_tx_logfn g_log = NULL;
void cq_tx_set_log(cq_tx_logfn fn) { g_log = fn; (void)g_log; }

void cq_tx_udp(const char *host, int port, const void *data, size_t len)
{
    static int ufd = -2;                 /* -2 untried, -1 dead, else the socket */
    struct sockaddr_in to;

    if (!host || !data || len == 0 || ufd == -1) return;
    if (ufd == -2) {
        ufd = socket(AF_INET, SOCK_DGRAM, 0);
        if (ufd < 0) { ufd = -1; return; }
    }
    memset(&to, 0, sizeof(to));
    to.sin_family = AF_INET;
    to.sin_port   = htons((unsigned short)port);
    if (inet_pton(AF_INET, host, &to.sin_addr) != 1) return;
    if (sendto(ufd, data, len, 0, (struct sockaddr *)&to, sizeof(to)) == (ssize_t)len)
        g_udp_ok++;
    else { g_udp_fail++; g_udp_lasterr = errno; }
}

#endif /* !CQ_OS9 */
