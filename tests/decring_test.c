#include "cq_test.h"
#include "cq_decring.h"
#include "cq_mp3.h"
#include "cq_mp3dec.h"

#include <stdlib.h>
#include <string.h>

/*
 * The decode pipeline (comp SPSC ring -> stage -> minimp3 -> PCM ring) is
 * exercised single-threaded: producer puts and consumer pumps alternate in
 * one thread, which is exactly the fallback path on OS 9 and a faithful
 * interleaving of the b52 task path (every cursor has one writer either way).
 * Equivalence oracle: the same fixture decoded flat (the mp3dec_test walk)
 * must yield byte-identical PCM — checked with a rolling FNV-1a hash, since
 * MP3's bit reservoir makes PCM depend on every prior frame.
 */

static unsigned long long hash_bytes(unsigned long long h,
                                     const unsigned char *p, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 1099511628211ULL;   /* FNV-1a 64 */
    }
    return h;
}

/* Producer helper: push up to n bytes through claim/commit (wrap-aware);
 * returns how many actually fit. */
static unsigned long ring_put(cq_decring *r, const unsigned char *p,
                              unsigned long n)
{
    unsigned long done = 0;
    while (done < n) {
        unsigned long contig = 0;
        unsigned char *dst = cq_decring_claim(r, &contig);
        if (!contig) break;
        if (contig > n - done) contig = n - done;
        memcpy(dst, p + done, contig);
        cq_decring_commit(r, contig);
        done += contig;
    }
    return done;
}

/* Fake PCM sink: consume everything available, hashing it in stream order. */
static long pcm_drain(const unsigned char *pcm, unsigned long size,
                      volatile unsigned long *wr, volatile unsigned long *rd,
                      unsigned long long *h)
{
    unsigned long avail = *wr - *rd;
    unsigned long done = 0;
    while (done < avail) {
        unsigned long off   = (*rd + done) % size;
        unsigned long first = size - off;
        if (first > avail - done) first = avail - done;
        *h = hash_bytes(*h, pcm + off, (size_t)first);
        done += first;
    }
    *rd += avail;
    return (long)avail;
}

/* Pump + drain until neither moves (gate held / everything decoded). */
static void pump_dry(cq_decring *r, const unsigned char *pcm,
                     unsigned long size, volatile unsigned long *wr,
                     volatile unsigned long *rd, unsigned long long *h)
{
    for (;;) {
        int  p = cq_decring_pump(r);
        long d = pcm_drain(pcm, size, wr, rd, h);
        if (!p && !d) break;
    }
}

/* Flat-decode oracle: frames + FNV hash of all PCM, mp3dec_test style. */
static long flat_decode(const unsigned char *d, size_t len,
                        unsigned long long *h, long *pcm_bytes)
{
    static short pcm[CQ_MP3DEC_MAX_SAMPLES];
    size_t off = 0;
    long   frames = 0;
    *pcm_bytes = 0;
    cq_mp3dec_init();
    while (off < len) {
        int consumed = 0, fch = 0, fhz = 0;
        int s = cq_mp3dec_frame(d + off, len - off, pcm, &fch, &fhz, &consumed);
        if (consumed <= 0) break;
        off += (size_t)consumed;
        if (s <= 0) continue;
        frames++;
        *h = hash_bytes(*h, (const unsigned char *)pcm,
                        (size_t)s * fch * 2);
        *pcm_bytes += (long)s * fch * 2;
    }
    return frames;
}

static unsigned long long fake_now_calls = 0;
static unsigned long long fake_now(void) { return ++fake_now_calls; }

void decring_tests(void)
{
    size_t len = 0;
    unsigned char *d;

    printf("decring\n");

    d = cq_fixture("stream.mp3", &len);
    if (!d) return;   /* fixtures unset: self-skip like the other suites */

    /* The app enters the pump only after AU_SYNC found the first confirmed
     * frame, so every stream test feeds from the fixture's sync offset (the
     * raw capture carries ICY-era junk at the head). The capture also ends
     * mid-frame; trim to whole frames so "everything decoded" is decidable —
     * a trailing partial is held forever by design, exactly like today's
     * gCompLen with a dead stream. */
    {
        cq_mp3_frame f0;
        int wf = 0;
        long so = cq_mp3_sync(d, len, &f0);
        CHECK(so >= 0, "fixture syncs");
        if (so > 0) { memmove(d, d + so, len - (size_t)so); len -= (size_t)so; }
        len = cq_mp3_walk(d, len, 1000000, &wf);
        CHECK(wf >= 50, "fixture holds 50+ whole frames");
    }

    {   /* equivalence: awkward feed sizes through the ring == flat decode */
        static unsigned char comp[64 * 1024];
        static unsigned char pcm[256 * 1024];
        static const unsigned long chunks[] = { 7, 1024, 17 * 1024 };
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long href = 14695981039346656037ULL, hring = href;
        long ref_bytes = 0, ref_frames;
        size_t off = 0;
        int i = 0;

        ref_frames = flat_decode(d, len, &href, &ref_bytes);

        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        cq_mp3dec_init();
        while (off < len) {
            unsigned long want = chunks[i++ % 3];
            if (want > len - off) want = (unsigned long)(len - off);
            off += ring_put(&ring, d + off, want);
            pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &hring);
        }
        cq_decring_eof(&ring);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &hring);

        CHECK(ring.frames == ref_frames, "ring decodes the same frame count as flat");
        CHECK(hring == href, "ring PCM is byte-identical to flat decode");
        CHECK(ring.pcm_total == ref_bytes, "pcm_total matches flat byte count");
        CHECK(cq_decring_idle(&ring), "ring is idle once everything decoded");
        CHECK(ring.resyncs == 0 && ring.flushes == 0,
              "clean stream needed no resync/flush");
    }

    {   /* tiny comp ring: many wraps, same bytes out */
        static unsigned char comp[16 * 1024];
        static unsigned char pcm[256 * 1024];
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long href = 14695981039346656037ULL, hring = href;
        long ref_bytes = 0, ref_frames;
        size_t off = 0;

        ref_frames = flat_decode(d, len, &href, &ref_bytes);

        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        cq_mp3dec_init();
        while (off < len) {
            off += ring_put(&ring, d + off, (unsigned long)(len - off));
            pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &hring);
        }
        cq_decring_eof(&ring);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &hring);

        CHECK(ring.frames == ref_frames, "wrapped comp ring: same frame count");
        CHECK(hring == href, "wrapped comp ring: byte-identical PCM");
    }

    {   /* b26 tail-gate: the final frame is held until eof flushes it.
         * Expectations come from the flat oracle over the same bytes, so the
         * test is immune to minimp3's warm-up (the first ~2 frames decode to
         * 0 samples while the bit reservoir fills). */
        static unsigned char comp[64 * 1024];
        static unsigned char pcm[256 * 1024];
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long h = 0;
        long   f_all, junk_bytes;
        int    walked = 0;
        size_t n6;

        n6 = cq_mp3_walk(d, len, 6, &walked);
        CHECK(walked == 6, "fixture holds 6 whole frames");
        f_all = flat_decode(d, n6, &h, &junk_bytes);
        CHECK(f_all >= 3, "6 fed frames yield PCM past the warm-up");

        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        cq_mp3dec_init();
        ring_put(&ring, d, (unsigned long)n6);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);
        CHECK(ring.frames == f_all - 1, "gate holds the unconfirmed final frame");
        CHECK(!cq_decring_idle(&ring), "held tail still staged");

        cq_decring_eof(&ring);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);
        CHECK(cq_decring_idle(&ring), "eof flushes the held tail");
        CHECK(ring.frames == f_all, "flushed tail completes the flat count");
    }

    {   /* b27 resync: splice junk between two synced runs must be skipped */
        static unsigned char comp[128 * 1024];
        static unsigned char pcm[512 * 1024];
        static unsigned char buf[96 * 1024];
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long h = 0;
        cq_mp3_frame f;
        long   sync_off = cq_mp3_sync(d, len, &f);
        int    walked = 0;
        size_t nA = cq_mp3_walk(d + sync_off, len - (size_t)sync_off, 8, &walked);
        size_t pos = 0;
        long   framesA;

        CHECK(walked == 8, "fixture holds 8 whole frames");
        memcpy(buf + pos, d + sync_off, nA);           pos += nA;
        memset(buf + pos, 0x55, 9216);                 pos += 9216;   /* > the 8 KB gate */
        memcpy(buf + pos, d + sync_off, nA);           pos += nA;

        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        cq_mp3dec_init();
        ring_put(&ring, buf, (unsigned long)nA);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);
        framesA = ring.frames;

        ring_put(&ring, buf + nA, (unsigned long)(pos - nA));
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);
        cq_decring_eof(&ring);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);

        CHECK(ring.resyncs >= 1, "splice junk triggered a resync");
        CHECK(ring.resync_bytes >= 9000, "the junk run was dropped");
        CHECK(ring.frames > framesA, "decode continued past the splice");
    }

    {   /* unparseable staging (> 8 KB, no frame anywhere) is flushed */
        static unsigned char comp[32 * 1024];
        static unsigned char pcm[64 * 1024];
        static unsigned char junk[9216];
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long h = 0;

        memset(junk, 0x55, sizeof junk);
        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        cq_mp3dec_init();
        ring_put(&ring, junk, sizeof junk);
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);
        CHECK(ring.flushes == 1, "unparseable staging flushed once");
        CHECK(cq_decring_idle(&ring), "flush drained the stage");
        CHECK(ring.frames == 0, "no frames conjured from junk");
    }

    {   /* backpressure both ways: a full PCM ring stalls the pump without
         * overwrite; a full comp ring clamps claim */
        static unsigned char comp[4 * 1024];
        static unsigned char pcm[8 * 1024];   /* one 4608 B frame + change */
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long h = 0;
        unsigned long contig = 0, put;

        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        cq_mp3dec_init();
        put = ring_put(&ring, d, (unsigned long)len);
        CHECK(put == sizeof comp, "comp ring clamps the put at its size");
        cq_decring_claim(&ring, &contig);
        CHECK(contig == 0 && cq_decring_space(&ring) == 0,
              "full comp ring claims nothing");

        cq_decring_pump(&ring);
        CHECK(ring.frames == 1, "full PCM ring stalls after one frame");
        CHECK(wr - rd == 4608, "exactly one frame in the PCM ring");

        pcm_drain(pcm, sizeof pcm, &wr, &rd, &h);   /* consumer catches up */
        cq_decring_pump(&ring);
        CHECK(ring.frames > 1, "pump resumes once the PCM ring drains");
    }

    {   /* injected clock accumulates decoder time */
        static unsigned char comp[64 * 1024];
        static unsigned char pcm[256 * 1024];
        volatile unsigned long wr = 0, rd = 0;
        cq_decring ring;
        unsigned long long h = 0;

        cq_decring_init(&ring, comp, sizeof comp, pcm, sizeof pcm, &wr, &rd);
        ring.now = fake_now;
        cq_mp3dec_init();
        ring_put(&ring, d, (unsigned long)(len < 32768 ? len : 32768));
        pump_dry(&ring, pcm, sizeof pcm, &wr, &rd, &h);
        CHECK(ring.frames > 0 && ring.dec_time > 0,
              "now() hook accumulates decode time");
    }

    free(d);
}
