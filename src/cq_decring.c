/*
 * cq_decring — compressed-MP3 SPSC ring + the decode pump (see cq_decring.h).
 *
 * The pump body is the b25 PumpRing lifted from casquinha.c with its scars
 * intact: the b26 tail-gate, the b27 head-resync, the b31 no-latency-gate
 * rule. DbgLog calls became counters (the event loop narrates); TickCount
 * became the injected now() hook (an MP task may not trap).
 */
#include "cq_decring.h"

#include <string.h>

#include "cq_mp3.h"

/* Compiler barrier: data must land before the cursor publishes it. GCC may
 * inline memcpy as a builtin, so "calls clobber memory" can't be relied on.
 * Uniprocessor OS 9 (and the single-threaded host suite) need no hardware
 * barrier. */
#if defined(__GNUC__)
#define CQ_DEC_BARRIER() __asm__ __volatile__("" ::: "memory")
#else
#define CQ_DEC_BARRIER() ((void)0)
#endif

void cq_decring_init(cq_decring *r,
                     unsigned char *comp, unsigned long comp_size,
                     unsigned char *pcm, unsigned long pcm_size,
                     volatile unsigned long *pcm_wr,
                     volatile unsigned long *pcm_rd)
{
    memset(r, 0, sizeof(*r));
    r->comp      = comp;
    r->comp_size = comp_size;
    r->pcm       = pcm;
    r->pcm_size  = pcm_size;
    r->pcm_wr    = pcm_wr;
    r->pcm_rd    = pcm_rd;
}

unsigned long cq_decring_space(const cq_decring *r)
{
    return r->comp_size - (r->comp_wr - r->comp_rd);
}

unsigned char *cq_decring_claim(cq_decring *r, unsigned long *contig)
{
    unsigned long space = cq_decring_space(r);
    unsigned long wr    = r->comp_wr % r->comp_size;
    unsigned long lin   = r->comp_size - wr;      /* bytes before the wrap */
    *contig = (space < lin) ? space : lin;
    return r->comp + wr;
}

void cq_decring_commit(cq_decring *r, unsigned long n)
{
    CQ_DEC_BARRIER();          /* the bytes land before the cursor says so */
    r->comp_wr += n;
}

void cq_decring_eof(cq_decring *r)
{
    r->eof = 1;
}

int cq_decring_idle(const cq_decring *r)
{
    return r->comp_wr == r->comp_rd && r->stage_len == 0;
}

void cq_decring_reset(cq_decring *r)
{
    r->comp_wr = r->comp_rd = 0;
    r->eof = 0;
    r->stage_len = 0;
    r->frames = r->resyncs = r->resync_bytes = r->flushes = r->pcm_total = 0;
    r->dec_time = 0;
}

int cq_decring_pump(cq_decring *r)
{
    int    progress = 0;
    size_t used     = 0;

    {   /* top up the private stage from the comp ring (wrap-aware) */
        unsigned long avail = r->comp_wr - r->comp_rd;
        unsigned long room  = (unsigned long)(CQ_DECRING_STAGE - r->stage_len);
        unsigned long take  = (avail < room) ? avail : room;
        if (take) {
            unsigned long rd    = r->comp_rd % r->comp_size;
            unsigned long first = r->comp_size - rd;
            if (first > take) first = take;
            memcpy(r->stage + r->stage_len, r->comp + rd, (size_t)first);
            if (take > first)
                memcpy(r->stage + r->stage_len + first, r->comp,
                       (size_t)(take - first));
            r->stage_len += (size_t)take;
            CQ_DEC_BARRIER();
            r->comp_rd += take;
            progress = 1;
        }
    }

    /* Decode staged MP3 into the PCM ring, one frame per iteration,
     * wrap-aware. The decoder skips junk itself; a consumed count of 0 means
     * "partial frame tail — wait for more bytes". */
    for (;;) {
        unsigned long space = r->pcm_size - (*r->pcm_wr - *r->pcm_rd);
        int consumed = 0, fch = 0, fhz = 0, s;
        /* NO latency gate here (b31): pausing the decoder just parks the
         * backlog upstream where it can't be trimmed. Decode everything;
         * the trim at the PCM ring bounds the latency. */
        if (space < (unsigned long)(CQ_MP3DEC_MAX_SAMPLES * 2)) break;  /* ring full */
        if (used >= r->stage_len) break;                                /* stage dry */
        {   /* NEVER feed the decoder an unconfirmed tail: minimp3 treats a
             * final frame it cannot verify against the NEXT header as junk
             * and EATS it silently (b26: ~40% of the input vanished this
             * way). Hold the tail until its successor is fully staged; a
             * dead stream (eof) flushes its true last frame. */
            int wf = 0;
            cq_mp3_walk(r->stage + used, r->stage_len - used, 2, &wf);
            if (wf < (r->eof ? 1 : 2)) {
                /* Gate failing at the HEAD with a deep stage = junk from a
                 * mid-stream splice, and the gate is blocking the very
                 * resync that would skip it — b27 deadlocked here for 20 s.
                 * Realign explicitly and drop the prefix. */
                if (r->stage_len - used > 8192) {
                    cq_mp3_frame nf;
                    long off = cq_mp3_sync(r->stage + used,
                                           r->stage_len - used, &nf);
                    if (off > 0) {
                        r->resyncs++;
                        r->resync_bytes += off;
                        used += (size_t)off;
                        continue;
                    }
                    if (off < 0) {
                        r->flushes++;
                        used = r->stage_len;
                    }
                }
                break;
            }
        }
        if (r->now) {   /* decode-throughput probe: raw now() units integrated
                         * over many frames tell whether the decoder keeps up
                         * with realtime (38 fps on emulated PPC) */
            unsigned long long t0 = r->now();
            s = cq_mp3dec_frame(r->stage + used, r->stage_len - used,
                                r->frame, &fch, &fhz, &consumed);
            r->dec_time += r->now() - t0;
        } else {
            s = cq_mp3dec_frame(r->stage + used, r->stage_len - used,
                                r->frame, &fch, &fhz, &consumed);
        }
        if (consumed <= 0) break;                  /* partial tail: need bytes */
        used += (size_t)consumed;
        progress = 1;
        if (s <= 0) continue;                      /* junk skipped */
        r->frames++;
        {
            unsigned long bytes = (unsigned long)s * fch * 2;
            unsigned long wr    = *r->pcm_wr % r->pcm_size;
            unsigned long first = r->pcm_size - wr;
            if (first > bytes) first = bytes;
            memcpy(r->pcm + wr, r->frame, (size_t)first);
            if (bytes > first)
                memcpy(r->pcm, (char *)r->frame + first,
                       (size_t)(bytes - first));
            CQ_DEC_BARRIER();  /* PCM lands before the interrupt may read it */
            *r->pcm_wr += bytes;
            r->pcm_total += (long)bytes;
        }
    }
    if (used) {
        r->stage_len -= used;
        if (r->stage_len) memmove(r->stage, r->stage + used, r->stage_len);
    }
    return progress;
}
