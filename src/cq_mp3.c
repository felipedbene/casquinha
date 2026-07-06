#include "cq_mp3.h"

/* Layer III bitrates (kbps) by index; 0 = free format (rejected), 15 = bad. */
static const int kBitV1[16] = { 0, 32, 40, 48, 56, 64, 80, 96,
                                112, 128, 160, 192, 224, 256, 320, 0 };
static const int kBitV2[16] = { 0, 8, 16, 24, 32, 40, 48, 56,
                                64, 80, 96, 112, 128, 144, 160, 0 };

/* Sample rates (Hz) by index for MPEG 1; V2 halves, V2.5 quarters. */
static const int kRate[4] = { 44100, 48000, 32000, 0 };

int cq_mp3_parse(const unsigned char *p, size_t n, cq_mp3_frame *f)
{
    int vbits, lbits, bidx, ridx, pad, mode;
    int version, bitrate, rate;

    if (!p || n < 4 || !f) return 0;
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return 0;   /* 11-bit sync */

    vbits = (p[1] >> 3) & 3;    /* 0=V2.5, 1=reserved, 2=V2, 3=V1 */
    lbits = (p[1] >> 1) & 3;    /* 1=Layer III */
    if (vbits == 1 || lbits != 1) return 0;
    version = (vbits == 3) ? 1 : (vbits == 2) ? 2 : 25;

    bidx = (p[2] >> 4) & 0x0F;
    ridx = (p[2] >> 2) & 3;
    pad  = (p[2] >> 1) & 1;
    mode = (p[3] >> 6) & 3;     /* 3 = single channel */

    bitrate = (version == 1) ? kBitV1[bidx] : kBitV2[bidx];
    rate    = kRate[ridx];
    if (bitrate == 0 || rate == 0) return 0;
    if (version == 2)  rate /= 2;
    if (version == 25) rate /= 4;

    f->version      = version;
    f->bitrate_kbps = bitrate;
    f->samplerate   = rate;
    f->channels     = (mode == 3) ? 1 : 2;
    f->samples      = (version == 1) ? 1152 : 576;
    /* bytes = samples/8 * bitrate / rate + padding (integer floor) */
    f->frame_bytes  = (f->samples / 8) * (bitrate * 1000) / rate + pad;
    if (f->frame_bytes < 4) return 0;
    return 1;
}

size_t cq_mp3_walk(const unsigned char *buf, size_t n, int max_frames, int *frames)
{
    size_t off = 0;
    int cnt = 0;
    cq_mp3_frame first, f;

    if (frames) *frames = 0;
    if (!buf || !frames || max_frames <= 0) return 0;
    while (cnt < max_frames) {
        if (!cq_mp3_parse(buf + off, n - off, &f)) break;
        if (off + (size_t)f.frame_bytes > n) break;   /* partial tail */
        if (cnt == 0) first = f;
        else if (f.version != first.version || f.samplerate != first.samplerate)
            break;
        off += (size_t)f.frame_bytes;
        cnt++;
    }
    *frames = cnt;
    return off;
}

long cq_mp3_sync(const unsigned char *buf, size_t n, cq_mp3_frame *f)
{
    size_t i;
    cq_mp3_frame a, b;

    if (!buf || n < 4 || !f) return -1;
    for (i = 0; i + 4 <= n; i++) {
        if (buf[i] != 0xFF) continue;
        if (!cq_mp3_parse(buf + i, n - i, &a)) continue;
        /* Trust it only when the header it predicts confirms it. */
        if (i + (size_t)a.frame_bytes + 4 > n) return -1;   /* need more bytes */
        if (!cq_mp3_parse(buf + i + a.frame_bytes, n - i - a.frame_bytes, &b))
            continue;                                       /* false sync */
        if (b.version != a.version || b.samplerate != a.samplerate)
            continue;                                       /* incoherent pair */
        *f = a;
        return (long)i;
    }
    return -1;
}
