/* The ONLY translation unit that includes the minimp3 implementation. */
#define MINIMP3_IMPLEMENTATION
#define MINIMP3_NO_SIMD          /* generic C path — classic PPC, no SSE/NEON */

#include "minimp3.h"
#include "cq_mp3dec.h"

static mp3dec_t g_dec;

void cq_mp3dec_init(void)
{
    mp3dec_init(&g_dec);
}

int cq_mp3dec_frame(const unsigned char *buf, size_t n,
                    short *pcm, int *channels, int *hz, int *consumed)
{
    mp3dec_frame_info_t info;
    int samples;

    if (consumed) *consumed = 0;
    if (!buf || n == 0 || !pcm) return 0;
    samples = mp3dec_decode_frame(&g_dec, buf, (int)n, pcm, &info);
    if (consumed) *consumed = info.frame_bytes;
    if (channels) *channels = info.channels;
    if (hz)       *hz       = info.hz;
    return samples;
}
