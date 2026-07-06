/*
 * cq_mp3dec — Casquinha MP3 decode seam (minimp3 behind a thin API).
 *
 * QuickTime 5's SoundConverter assembles MP3 chains that consume input and
 * decode NOTHING (b13–b20 exhausted every QT route: URL movie, pull, push,
 * both fourccs). So the decode step is ours: minimp3 (public domain, vendored
 * verbatim as src/minimp3.h) compiled in exactly one translation unit behind
 * this header. Pure C, no OS calls — host-tested against a real captured
 * Icecast slice (tests/Fixtures/stream.mp3), same discipline as cq_codec.
 *
 * Single-stream by design: one static decoder state, reset via init. The app
 * plays at most one stream, and the suite is single-threaded.
 */
#ifndef CQ_MP3DEC_H
#define CQ_MP3DEC_H

#include <stddef.h>

/* Interleaved s16 output for one MPEG frame: up to 1152 samples x 2 ch. */
#define CQ_MP3DEC_MAX_SAMPLES (1152 * 2)

/* Reset decoder state (call once per new stream). */
void cq_mp3dec_init(void);

/*
 * Decode the first frame found in buf (leading junk is skipped). Writes up to
 * CQ_MP3DEC_MAX_SAMPLES interleaved s16 samples into pcm and returns samples
 * PER CHANNEL (0 = nothing decoded: junk skipped or more bytes needed).
 * *consumed gets the bytes to drop from buf — when it comes back 0, the tail
 * is a partial frame: accumulate more and retry.
 */
int cq_mp3dec_frame(const unsigned char *buf, size_t n,
                    short *pcm, int *channels, int *hz, int *consumed);

#endif /* CQ_MP3DEC_H */
