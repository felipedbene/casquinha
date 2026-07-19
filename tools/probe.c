/*
 * probe — a host-side end-to-end check of the Transport + Codec + Model stack
 * against the REAL gopher-spot server (the closest thing to portkit for the C
 * port). Fetches /spot/api/1/now and prints the parsed snapshot.
 *
 *   make probe                 # 192.0.2.10:70
 *   build/probe <host> <port> [selector]
 *
 * This is the same seam the OS 9 app will drive; here it runs on the host with
 * the POSIX transport, so a green probe proves the wire contract before any
 * Retro68/UTM build exists.
 */
#include "cq_transport.h"
#include "cq_now.h"
#include "cq_codec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    const char *host = argc > 1 ? argv[1] : "192.0.2.10";
    int         port = argc > 2 ? atoi(argv[2]) : 70;
    const char *sel  = argc > 3 ? argv[3] : "/spot/api/1/now";
    cq_transport *t;
    cq_tx_status st;
    int i;

    printf("probe %s:%d %s\n", host, port, sel);
    t = cq_tx_new(host, port, sel);
    if (!t) { fprintf(stderr, "out of memory\n"); return 2; }

    cq_tx_start(t);
    for (i = 0; i < 10000; i++) {
        st = cq_tx_poll(t);
        if (st != CQ_TX_RUNNING) break;
        usleep(1000);
    }

    if (st != CQ_TX_DONE) {
        fprintf(stderr, "FAILED (%d): %s\n", (int)cq_tx_error_code(t), cq_tx_error_message(t));
        cq_tx_free(t);
        return 1;
    }

    {
        size_t len = 0;
        const unsigned char *d = cq_tx_data(t, &len);
        cq_now n;
        cq_now_from_response(&n, d, len);
        printf("read %lu bytes\n", (unsigned long)len);
        printf("  api       %d\n",  n.api_version);
        printf("  state     %s\n",  n.state == CQ_STATE_PLAYING ? "playing" :
                                    n.state == CQ_STATE_PAUSED  ? "paused"  : "stopped");
        printf("  track     %s\n",  n.track  ? n.track  : "(none)");
        printf("  artist    %s\n",  n.artist ? n.artist : "(none)");
        printf("  album     %s\n",  n.album  ? n.album  : "(none)");
        printf("  device    %s\n",  n.device == CQ_DEV_ACTIVE ? "active" :
                                    n.device == CQ_DEV_IDLE   ? "idle"   : "unknown");
        printf("  volume    %d\n",  n.volume);
        printf("  ts        %lld\n", n.ts);
        cq_now_free(&n);
    }
    cq_tx_free(t);
    return 0;
}
