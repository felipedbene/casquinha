/*
 * cq_decring — Casquinha's decode pipeline: a compressed-MP3 SPSC ring feeding
 * an MP3→PCM pump (exp/mp-decode, b51).
 *
 * This is the b25 PumpRing body extracted from casquinha.c so ONE decode code
 * path can run in TWO execution contexts: inline on the cooperative event
 * loop (the fallback, exactly today's behavior) or on a preemptive MPLibrary
 * task (b52 — the ring keeps filling while menu tracking freezes the loop).
 * Pure C, no OS calls, no logging: host-tested single-threaded in
 * tests/decring_test.c, and MP-task-legal by construction (an MP task gets no
 * Toolbox, no Memory Manager, no Open Transport).
 *
 * Shape (same discipline as the b25 PCM ring — races impossible by ownership,
 * not by locks; every cursor is a monotonic byte counter with ONE writer):
 *
 *   producer (event loop)          consumer (task or loop)      interrupt
 *   cq_tx_drain ─► comp ring ─► private stage ─► mp3dec ─► PCM ring ─► SndDoubleBuffer
 *                  comp_wr: producer  comp_rd: consumer   pcm_wr: consumer  pcm_rd: interrupt
 *
 * The consumer-private flat `stage` exists because cq_mp3_walk/cq_mp3dec_frame
 * need contiguous bytes; it keeps today's memmove compaction. The decode loop
 * carries the b26 tail-gate (never feed the decoder an unverified final
 * frame), the b27 head-resync (splice junk must not deadlock the gate), and
 * the b31 lesson (NO latency gate in the decoder — the trim lives at the PCM
 * ring where backlog is measurable).
 *
 * Cross-context rules:
 *  - comp_wr/eof: producer writes.  comp_rd/stage/counters/pcm_wr: consumer
 *    writes.  pcm_rd: the Sound interrupt writes.  Everyone else only reads,
 *    and a stale read of a monotonic counter is always conservative.
 *  - CQ_DEC_BARRIER() (compiler barrier) runs before each cursor publication
 *    so data lands before the counter says it did; uniprocessor OS 9 needs no
 *    hardware barrier.
 *  - cq_decring_reset() is legal ONLY while the consumer is provably parked
 *    (or is the caller). The decoder itself (cq_mp3dec) is single-stream
 *    static state owned by whichever context runs the pump.
 *  - No logging in here: decode events land in counters the event loop polls
 *    and narrates (DbgLog is Toolbox+OT, illegal on an MP task).
 */
#ifndef CQ_DECRING_H
#define CQ_DECRING_H

#include <stddef.h>

#include "cq_mp3dec.h"

/* Consumer-private staging: same 64 KB decode workspace as the b16 gComp. */
#define CQ_DECRING_STAGE (64 * 1024)

typedef struct {
    /* compressed SPSC ring (producer: event loop; consumer: the pump) */
    unsigned char          *comp;
    unsigned long           comp_size;
    volatile unsigned long  comp_wr;    /* bytes written — producer only */
    volatile unsigned long  comp_rd;    /* bytes consumed — consumer only */
    volatile int            eof;        /* producer: stream died, flush the
                                           held tail (b26 gate 2 -> 1) */

    /* consumer-private decode workspace (never touched by the producer) */
    unsigned char stage[CQ_DECRING_STAGE];
    size_t        stage_len;
    short         frame[CQ_MP3DEC_MAX_SAMPLES];  /* one decoded frame, bounced
                                                    into the PCM ring with wrap */

    /* PCM output: points at the app's existing ring + cursors so the
     * interrupt-time double-back proc stays untouched */
    unsigned char          *pcm;
    unsigned long           pcm_size;
    volatile unsigned long *pcm_wr;     /* consumer publishes (barrier first) */
    volatile unsigned long *pcm_rd;     /* Sound interrupt owns; racy reads
                                           under-estimate space = safe */

    /* telemetry mailbox: consumer writes, event loop polls + narrates */
    volatile long frames;               /* MP3 frames decoded */
    volatile long resyncs;              /* b27 head-realignments */
    volatile long resync_bytes;         /* splice junk dropped, total */
    volatile long flushes;              /* unparseable staging discarded */
    volatile long pcm_total;            /* PCM bytes produced */
    volatile unsigned long long dec_time;  /* time inside the decoder, in raw
                                              now() units (ticks on the loop,
                                              UpTime on an MP task) */
    unsigned long long (*now)(void);    /* injected clock; NULL = no timing.
                                           MUST be legal in the consumer's
                                           context (no Toolbox on a task). */
} cq_decring;

/* Wire the struct to its buffers and zero all state. pcm_wr/pcm_rd are the
 * app's existing volatile PCM cursors (interrupt contract unchanged). */
void cq_decring_init(cq_decring *r,
                     unsigned char *comp, unsigned long comp_size,
                     unsigned char *pcm, unsigned long pcm_size,
                     volatile unsigned long *pcm_wr,
                     volatile unsigned long *pcm_rd);

/* --- producer side (event loop) --- */

/* Writable bytes left in the comp ring (racy-safe from anywhere). */
unsigned long cq_decring_space(const cq_decring *r);

/* Zero-copy put: returns the write position and sets *contig to the bytes
 * writable there without wrapping (0 = ring full). Write, then commit.
 * Call again after committing to reach the wrapped remainder. */
unsigned char *cq_decring_claim(cq_decring *r, unsigned long *contig);
void           cq_decring_commit(cq_decring *r, unsigned long n);

/* Stream died: let the pump flush the held final frame (b26). */
void cq_decring_eof(cq_decring *r);

/* --- consumer side (the pump; event loop in fallback, MP task in b52) --- */

/* One bounded, non-blocking pass: top up stage from the comp ring, then
 * decode staged frames into the PCM ring until the ring is full, the stage
 * runs dry, or the tail-gate holds. Returns nonzero if any progress was made
 * (bytes moved or frames decoded) — the task's idle tell. */
int cq_decring_pump(cq_decring *r);

/* Nothing left to decode: comp ring drained AND stage empty. */
int cq_decring_idle(const cq_decring *r);

/* Zero cursors/stage/counters/eof for a new stream. ONLY while the consumer
 * is parked (or is the caller) — see the cross-context rules above. */
void cq_decring_reset(cq_decring *r);

#endif /* CQ_DECRING_H */
