#include "cq_test.h"
#include "cq_mp3.h"

#include <string.h>

/* Lay a synthetic frame into buf at off: a real header, zero-filled body.
 * Returns the offset just past the frame. */
static size_t put_frame(unsigned char *buf, size_t off,
                        unsigned char b1, unsigned char b2, unsigned char b3,
                        int frame_bytes)
{
    memset(buf + off, 0, (size_t)frame_bytes);
    buf[off]     = 0xFF;
    buf[off + 1] = b1;
    buf[off + 2] = b2;
    buf[off + 3] = b3;
    return off + (size_t)frame_bytes;
}

void mp3_tests(void)
{
    printf("mp3\n");

    /* MPEG1 Layer III, 160 kbps, 44100 Hz, stereo, no padding:
     * FF FB A0 00 -> 144 * 160000 / 44100 = 522 bytes, 1152 samples. */
    { cq_mp3_frame f;
      unsigned char h[4] = { 0xFF, 0xFB, 0xA0, 0x00 };
      CHECK(cq_mp3_parse(h, 4, &f) == 1, "V1 L3 header parses");
      CHECK(f.version == 1,        "V1 version");
      CHECK(f.bitrate_kbps == 160, "160 kbps");
      CHECK(f.samplerate == 44100, "44100 Hz");
      CHECK(f.channels == 2,       "stereo");
      CHECK(f.samples == 1152,     "1152 samples/frame");
      CHECK(f.frame_bytes == 522,  "522-byte frame"); }

    /* Same with the padding bit: FF FB A2 00 -> 523. */
    { cq_mp3_frame f;
      unsigned char h[4] = { 0xFF, 0xFB, 0xA2, 0x00 };
      CHECK(cq_mp3_parse(h, 4, &f) == 1, "padded header parses");
      CHECK(f.frame_bytes == 523, "padding adds one byte"); }

    /* 320 kbps mono: FF FB E0 C0 -> 144 * 320000 / 44100 = 1044. */
    { cq_mp3_frame f;
      unsigned char h[4] = { 0xFF, 0xFB, 0xE0, 0xC0 };
      CHECK(cq_mp3_parse(h, 4, &f) == 1, "320 kbps header parses");
      CHECK(f.bitrate_kbps == 320, "320 kbps");
      CHECK(f.channels == 1,       "mono");
      CHECK(f.frame_bytes == 1044, "1044-byte frame"); }

    /* MPEG2 Layer III, 64 kbps, 22050 Hz: FF F3 80 00 ->
     * 72 * 64000 / 22050 = 208 bytes, 576 samples. */
    { cq_mp3_frame f;
      unsigned char h[4] = { 0xFF, 0xF3, 0x80, 0x00 };
      CHECK(cq_mp3_parse(h, 4, &f) == 1, "V2 L3 header parses");
      CHECK(f.version == 2,       "V2 version");
      CHECK(f.samplerate == 22050, "22050 Hz");
      CHECK(f.samples == 576,     "576 samples/frame");
      CHECK(f.frame_bytes == 208, "208-byte frame"); }

    /* Rejects: bad sync, reserved version, Layer II, free-format, bad rate. */
    { cq_mp3_frame f;
      unsigned char bad1[4] = { 0xFE, 0xFB, 0xA0, 0x00 };   /* no sync */
      unsigned char bad2[4] = { 0xFF, 0xEB, 0xA0, 0x00 };   /* reserved version */
      unsigned char bad3[4] = { 0xFF, 0xFD, 0xA0, 0x00 };   /* Layer II */
      unsigned char bad4[4] = { 0xFF, 0xFB, 0x00, 0x00 };   /* free format */
      unsigned char bad5[4] = { 0xFF, 0xFB, 0xAC, 0x00 };   /* rate idx 3 */
      CHECK(cq_mp3_parse(bad1, 4, &f) == 0, "no sync rejected");
      CHECK(cq_mp3_parse(bad2, 4, &f) == 0, "reserved version rejected");
      CHECK(cq_mp3_parse(bad3, 4, &f) == 0, "Layer II rejected");
      CHECK(cq_mp3_parse(bad4, 4, &f) == 0, "free format rejected");
      CHECK(cq_mp3_parse(bad5, 4, &f) == 0, "bad rate index rejected");
      CHECK(cq_mp3_parse(NULL, 4, &f) == 0, "NULL rejected");
      CHECK(cq_mp3_parse(bad1, 3, &f) == 0, "short buffer rejected"); }

    /* Sync: two coherent frames after leading junk (junk includes a false
     * 0xFF that must be skipped). */
    { unsigned char buf[2048];
      cq_mp3_frame f;
      size_t off;
      long r;
      memset(buf, 0xAA, sizeof(buf));
      buf[3] = 0xFF;                      /* bare false sync, no valid header */
      off = put_frame(buf, 10, 0xFB, 0xA0, 0x00, 522);
      off = put_frame(buf, off, 0xFB, 0xA0, 0x00, 522);
      r = cq_mp3_sync(buf, off, &f);
      CHECK(r == 10, "sync lands on the first real frame");
      CHECK(f.frame_bytes == 522, "sync fills the frame info"); }

    /* Sync withholds judgment until the confirming header is in the buffer. */
    { unsigned char buf[600];
      cq_mp3_frame f;
      memset(buf, 0, sizeof(buf));
      put_frame(buf, 0, 0xFB, 0xA0, 0x00, 522);
      CHECK(cq_mp3_sync(buf, 300, &f) == -1, "half a frame: need more bytes");
      CHECK(cq_mp3_sync(buf, 524, &f) == -1, "frame + 2 bytes: still unconfirmed"); }

    /* A false sync whose 'next header' is garbage is skipped, and a real
     * frame later in the buffer is still found. */
    { unsigned char buf[2048];
      cq_mp3_frame f;
      size_t off;
      memset(buf, 0, sizeof(buf));
      buf[0] = 0xFF; buf[1] = 0xFB; buf[2] = 0xA0; buf[3] = 0x00;  /* valid header, garbage follow-up */
      off = put_frame(buf, 700, 0xFB, 0xA0, 0x00, 522);
      off = put_frame(buf, off, 0xFB, 0xA0, 0x00, 522);
      CHECK(cq_mp3_sync(buf, off, &f) == 700, "false sync skipped, real frame found"); }

    /* Walk: whole coherent frames only, capped, partial tail excluded. */
    { unsigned char buf[4096];
      size_t off;
      int frames;
      memset(buf, 0, sizeof(buf));
      off = put_frame(buf, 0, 0xFB, 0xA0, 0x00, 522);
      off = put_frame(buf, off, 0xFB, 0xA2, 0x00, 523);   /* padded frame mixes in */
      off = put_frame(buf, off, 0xFB, 0xA0, 0x00, 522);
      CHECK(cq_mp3_walk(buf, off, 16, &frames) == 522 + 523 + 522 &&
            frames == 3, "walk takes all three whole frames");
      CHECK(cq_mp3_walk(buf, off - 1, 16, &frames) == 522 + 523 &&
            frames == 2, "walk excludes the partial tail");
      CHECK(cq_mp3_walk(buf, off, 2, &frames) == 522 + 523 && frames == 2,
            "walk honors the frame cap");
      CHECK(cq_mp3_walk(buf, 3, 16, &frames) == 0 && frames == 0,
            "walk on a sliver: zero frames");
      buf[522] = 0x00;                                    /* break frame 2's sync */
      CHECK(cq_mp3_walk(buf, off, 16, &frames) == 522 && frames == 1,
            "walk stops at a broken header"); }

    /* Version-incoherent pair does not confirm. */
    { unsigned char buf[2048];
      cq_mp3_frame f;
      size_t off;
      memset(buf, 0, sizeof(buf));
      off = put_frame(buf, 0, 0xFB, 0xA0, 0x00, 522);   /* V1 ... */
      put_frame(buf, off, 0xF3, 0x80, 0x00, 208);        /* ...then V2: no */
      CHECK(cq_mp3_sync(buf, off + 208, &f) == -1, "incoherent pair not confirmed"); }
}
