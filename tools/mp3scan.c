/*
 * mp3scan — host-side mount forensics (the b24 "chops + overlaps" hunt).
 *
 * Reads a captured MP3 byte stream and walks it with cq_mp3: reports total
 * frames, audio seconds, junk gaps between frames (bytes skipped to regain
 * sync), and format flips (bitrate/samplerate/channel changes). A healthy
 * CBR Icecast mount is one unbroken run of identical-format frames; splices
 * from a restarting encoder show up as junk gaps or format flips.
 *
 *   cc -Isrc tools/mp3scan.c src/cq_mp3.c -o build/mp3scan
 *   build/mp3scan capture.mp3
 */
#include "cq_mp3.h"

#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    FILE *f;
    unsigned char *buf;
    long n, off = 0;
    long frames = 0, gaps = 0, gapBytes = 0, flips = 0;
    double secs = 0.0;
    cq_mp3_frame cur, prev;
    int havePrev = 0;

    if (argc != 2) { fprintf(stderr, "usage: mp3scan <capture.mp3>\n"); return 2; }
    f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 2; }
    fseek(f, 0, SEEK_END); n = ftell(f); fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { perror("read"); return 2; }
    fclose(f);

    while (off + 4 <= n) {
        if (!cq_mp3_parse(buf + off, (size_t)(n - off), &cur)) {
            /* lost sync: scan forward for the next confirmed frame */
            long next = cq_mp3_sync(buf + off, (size_t)(n - off), &cur);
            if (next < 0) break;              /* nothing but junk to the end */
            gaps++;
            gapBytes += next;
            printf("GAP   at %8ld: %ld junk bytes\n", off, next);
            off += next;
            continue;
        }
        if (off + cur.frame_bytes > n) break; /* partial tail */
        if (havePrev &&
            (cur.bitrate_kbps != prev.bitrate_kbps ||
             cur.samplerate   != prev.samplerate   ||
             cur.channels     != prev.channels)) {
            flips++;
            printf("FLIP  at %8ld: %d kbps %d Hz %dch -> %d kbps %d Hz %dch\n",
                   off, prev.bitrate_kbps, prev.samplerate, prev.channels,
                   cur.bitrate_kbps, cur.samplerate, cur.channels);
        }
        prev = cur; havePrev = 1;
        frames++;
        secs += (double)cur.samples / (double)cur.samplerate;
        off += cur.frame_bytes;
    }

    printf("----\n");
    printf("bytes %ld  frames %ld  audio %.2f s\n", n, frames, secs);
    printf("gaps %ld (%ld junk bytes)  format-flips %ld\n", gaps, gapBytes, flips);
    if (frames) {
        printf("first frame: %d kbps %d Hz %dch, %d-byte frames\n",
               cur.bitrate_kbps, cur.samplerate, cur.channels, cur.frame_bytes);
    }
    free(buf);
    return 0;
}
