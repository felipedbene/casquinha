/*
 * cq_mp3 — Casquinha MPEG Layer III frame-header reader (pure).
 *
 * The ⌘T audio path streams raw MP3 off Icecast and hands it to QuickTime's
 * SoundConverter, which needs the sample rate / channel count up front and a
 * clean frame boundary to start from. This module answers exactly that, from
 * bytes alone: parse one 4-byte frame header, and find the first TRUSTWORTHY
 * frame in a buffer (a candidate is trusted only when the header that its
 * frame length predicts also parses and agrees — a lone 0xFFEx match in
 * compressed audio is routinely a false sync).
 *
 * Pure C99, no I/O, no OS — same discipline as cq_codec/cq_pls; host-tested
 * in tests/mp3_test.c.
 */
#ifndef CQ_MP3_H
#define CQ_MP3_H

#include <stddef.h>

typedef struct {
    int version;       /* 1, 2, or 25 (MPEG 2.5) */
    int bitrate_kbps;  /* 8..320; free-format (0) is rejected */
    int samplerate;    /* Hz: 8000..48000 */
    int channels;      /* 1 or 2 */
    int samples;       /* PCM sample frames per MP3 frame: 1152 (V1) / 576 (V2, V2.5) */
    int frame_bytes;   /* whole frame length incl. header + padding */
} cq_mp3_frame;

/*
 * Parse the Layer III frame header at p (needs n >= 4). Returns 1 and fills
 * *f when it is a valid header; 0 otherwise. Free-format bitrate and the
 * reserved version/samplerate codes are rejected (never seen on Icecast, and
 * their frame length is unknowable here).
 */
int cq_mp3_parse(const unsigned char *p, size_t n, cq_mp3_frame *f);

/*
 * Find the first trustworthy frame in buf: a valid header whose predicted
 * next frame ALSO parses with the same version/layer/samplerate. Returns the
 * byte offset and fills *f, or -1 if none can be confirmed yet — the caller
 * simply accumulates more bytes and retries (the stream is endless).
 */
long cq_mp3_sync(const unsigned char *buf, size_t n, cq_mp3_frame *f);

/*
 * Walk WHOLE frames from the start of buf (assumed already synced): count
 * consecutive frames whose headers parse and whose version/samplerate agree
 * with the first, stopping at max_frames, at the first bad header, or at a
 * frame that would run past n. Returns the byte length of the walked prefix
 * and sets *frames (0 when not even one whole frame fits).
 */
size_t cq_mp3_walk(const unsigned char *buf, size_t n, int max_frames, int *frames);

#endif /* CQ_MP3_H */
