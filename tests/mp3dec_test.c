#include "cq_test.h"
#include "cq_mp3dec.h"

#include <stdlib.h>

/*
 * Decode a real slice of the live Icecast stream (tests/Fixtures/stream.mp3,
 * captured from gopher-spot's mount) and prove the decoder produces actual
 * PCM — the exact property QuickTime's SoundConverter silently lacked
 * (b13–b20: noErr, full-rate input, zero output).
 */
void mp3dec_tests(void)
{
    size_t len = 0;
    unsigned char *d;

    printf("mp3dec\n");

    d = cq_fixture("stream.mp3", &len);
    if (!d) return;   /* fixtures unset: self-skip like the other suites */

    {
        static short pcm[CQ_MP3DEC_MAX_SAMPLES];
        size_t off = 0;
        int frames = 0, ch = 0, hz = 0;
        int all1152 = 1;
        long energy = 0;

        cq_mp3dec_init();
        while (off < len && frames < 50) {
            int consumed = 0, fch = 0, fhz = 0;
            int s = cq_mp3dec_frame(d + off, len - off, pcm, &fch, &fhz, &consumed);
            if (consumed <= 0) break;              /* partial tail: done */
            off += (size_t)consumed;
            if (s <= 0) continue;                  /* junk skipped */
            frames++;
            ch = fch; hz = fhz;
            if (s != 1152) all1152 = 0;
            {
                int i;
                for (i = 0; i < s * fch; i++)
                    energy += (pcm[i] < 0) ? -(long)pcm[i] : (long)pcm[i];
            }
        }
        CHECK(frames >= 20, "decoded 20+ frames from the live capture");
        CHECK(all1152, "every frame yielded 1152 samples per channel");
        CHECK(ch == 2, "stream is stereo");
        CHECK(hz == 44100, "stream is 44.1 kHz");
        CHECK(energy > 0, "PCM is not silence");
    }
    free(d);
}
