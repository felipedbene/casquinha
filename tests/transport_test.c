/*
 * transport_test — the Transport<->Codec seam, exercised offline.
 *
 * Mirrors DeGelato's DGGopherClientTests: spin up an in-process localhost
 * loopback server (no LAN, no real gopher-spot), serve a canned /now body, and
 * assert the client connects, writes "selector\r\n", reads to EOF, and hands the
 * exact bytes to the pure stack. Also checks a connection-refused failure.
 */
#define _DARWIN_C_SOURCE 1
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE 1
#endif

#include "cq_test.h"
#include "cq_transport.h"
#include "cq_now.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

static const char *BODY =
    "api\t1\r\nstate\tplaying\r\ntrack\tMamma Mia\r\nartist\tABBA\r\n"
    "volume\t100\r\nts\t1783131226118\r\n";

typedef struct {
    int  listen_fd;
    int  port;
    char got_selector[256];   /* the first line the server received */
} server_ctx;

/* Accept one connection, read the request line, echo the canned body, close. */
static void *serve_once(void *arg)
{
    server_ctx *s = (server_ctx *)arg;
    int fd = accept(s->listen_fd, NULL, NULL);
    char buf[256];
    ssize_t n;
    size_t i;
    if (fd < 0) return NULL;

    n = recv(fd, buf, sizeof(buf) - 1, 0);
    if (n > 0) {
        buf[n] = '\0';
        for (i = 0; i < (size_t)n && buf[i] != '\r' && buf[i] != '\n'; i++)
            s->got_selector[i] = buf[i];
        s->got_selector[i] = '\0';
    }
    send(fd, BODY, strlen(BODY), 0);
    close(fd);
    close(s->listen_fd);
    return NULL;
}

/* Bind 127.0.0.1:0, return the fd and the ephemeral port. */
static int start_server(server_ctx *s)
{
    struct sockaddr_in addr;
    socklen_t sl = sizeof(addr);
    int on = 1;

    s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (s->listen_fd < 0) return -1;
    setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port        = 0;
    if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) return -1;
    if (listen(s->listen_fd, 1) < 0) return -1;
    if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &sl) < 0) return -1;
    s->port = ntohs(addr.sin_port);
    s->got_selector[0] = '\0';
    return 0;
}

/* Drive a transaction to completion (bounded so a hang can't wedge the suite). */
static cq_tx_status drive(cq_transport *t)
{
    int i;
    cq_tx_start(t);
    for (i = 0; i < 10000; i++) {
        cq_tx_status st = cq_tx_poll(t);
        if (st != CQ_TX_RUNNING) return st;
        usleep(1000);
    }
    return CQ_TX_RUNNING;
}

void transport_tests(void);
void transport_tests(void)
{
    printf("transport\n");

    /* happy path: loopback server serves a /now body */
    {
        server_ctx s;
        pthread_t th;
        cq_transport *t;
        cq_tx_status st;

        CHECK(start_server(&s) == 0, "loopback server bound");
        pthread_create(&th, NULL, serve_once, &s);

        t = cq_tx_new("127.0.0.1", s.port, "/spot/api/1/now");
        st = drive(t);
        CHECK(st == CQ_TX_DONE, "transaction completed");

        {
            size_t len = 0;
            const unsigned char *d = cq_tx_data(t, &len);
            CHECK(len == strlen(BODY), "read the whole body to EOF");
            CHECK(d && memcmp(d, BODY, len) == 0, "bytes match verbatim");
            /* the seam: raw bytes -> pure model */
            if (d) {
                cq_now n;
                cq_now_from_response(&n, d, len);
                CHECK(n.state == CQ_STATE_PLAYING, "seam feeds the model: state playing");
                CHECK(n.volume == 100, "seam feeds the model: volume 100");
                cq_now_free(&n);
            }
        }
        pthread_join(th, NULL);
        CHECK_STR(s.got_selector, "/spot/api/1/now", "server received the exact selector");
        cq_tx_free(t);
    }

    /* connection refused: nothing listening on this port -> CONNECT error */
    {
        server_ctx s;
        cq_transport *t;
        cq_tx_status st;

        /* bind then immediately close to obtain a very-likely-free port */
        start_server(&s);
        close(s.listen_fd);

        t = cq_tx_new("127.0.0.1", s.port, "/spot/api/1/now");
        st = drive(t);
        CHECK(st == CQ_TX_FAILED, "refused connection fails");
        CHECK(cq_tx_error_code(t) == CQ_TX_ERR_CONNECT, "error is CONNECT");
        cq_tx_free(t);
    }
}
