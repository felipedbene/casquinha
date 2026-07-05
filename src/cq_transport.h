/*
 * cq_transport — Casquinha Transport layer (I/O only).
 *
 * One gopher transaction: connect to host:port, write "selector\r\n", read to
 * EOF, hand back the raw bytes. Nothing else — it never parses (that's cq_codec;
 * the Transport<->Codec seam is where the tests live, CLIENT-PATTERN.md §1).
 *
 * The interface is a POLL-DRIVEN STATE MACHINE, on purpose. Classic Mac OS is
 * cooperatively multitasked with no preemption, so the OS 9 (Open Transport)
 * implementation must advance in bounded slices from the WaitNextEvent loop and
 * never block the machine (NOTES.md). The host (POSIX) implementation used by
 * the tests satisfies the same interface by completing on the first poll.
 *
 * Two implementations, one header:
 *   cq_transport_posix.c   BSD sockets — host builds + the offline test.
 *   cq_transport_ot.c      Open Transport — the PowerPC / Mac OS 9 app.
 *
 * Deadlines are owned by the implementation: a LAN transaction that hasn't made
 * progress within ~2 s is dead; ~5 s is the outer watchdog. (Mirrors DeGelato's
 * DGGopherClient: DG_TIMEOUT_SECONDS 2, DG_WATCHDOG_SECONDS 5.)
 */
#ifndef CQ_TRANSPORT_H
#define CQ_TRANSPORT_H

#include <stddef.h>

#define CQ_TX_DEADLINE_SECS  2   /* connect + per-read LAN deadline */
#define CQ_TX_WATCHDOG_SECS  5   /* outer, whole-transaction watchdog */
#define CQ_TX_READ_CHUNK  8192

typedef enum {
    CQ_TX_RUNNING = 0,   /* still working — poll again next loop pass */
    CQ_TX_DONE    = 1,   /* bytes are ready: cq_tx_data() */
    CQ_TX_FAILED  = 2    /* see cq_tx_error_code() / _message() */
} cq_tx_status;

typedef enum {
    CQ_TX_ERR_NONE    = 0,
    CQ_TX_ERR_CONNECT = 1,   /* resolve / socket / connect refused */
    CQ_TX_ERR_TIMEOUT = 2,   /* connect or read exceeded the deadline */
    CQ_TX_ERR_STREAM  = 3    /* write or read failed mid-transaction */
} cq_tx_error;

typedef struct cq_transport cq_transport;   /* opaque; implementation-defined */

/*
 * Create a transaction (host/selector are copied; not started yet). A NULL
 * selector sends a bare "\r\n" (the RFC 1436 root request). Returns NULL on
 * allocation failure.
 */
cq_transport *cq_tx_new(const char *host, int port, const char *selector);

/* Begin. After this, drive it with cq_tx_poll() until it leaves RUNNING. */
void cq_tx_start(cq_transport *t);

/* Advance the state machine one bounded slice; returns the current status. */
cq_tx_status cq_tx_poll(cq_transport *t);

/* Response bytes, valid once status is DONE. *len gets the length (may be 0). */
const unsigned char *cq_tx_data(const cq_transport *t, size_t *len);

/* Error detail, valid once status is FAILED. */
cq_tx_error cq_tx_error_code(const cq_transport *t);
const char *cq_tx_error_message(const cq_transport *t);

/*
 * Stop listening. Per law 1 (cancel != un-send): if the selector already
 * reached the wire, the server still executes it — cancel only stops US. Reads
 * (idempotent polls) may be cancelled-and-replaced freely.
 */
void cq_tx_cancel(cq_transport *t);

void cq_tx_free(cq_transport *t);

#endif /* CQ_TRANSPORT_H */
