/*
 * cq_view — Casquinha View-model layer (pure): ONE function decides what the
 * player area says; the glue only draws.
 *
 * Fio B. Before this module, four code paths in os9/casquinha.c painted the
 * screen from four sources with four clocks: the state word (/now), the radio
 * readout (b45/b49: tuning in -> buffering N% -> on air / playing out /
 * standing by / waiting for Spotify), the "Skipping..." command ack (b48), and
 * the interpolated progress bar. They could — and did — disagree on screen.
 * All of that vocabulary now renders HERE, from one input struct, so a
 * contradiction is impossible by construction.
 *
 * The one genuinely new fact: /spot/api/1/stream (server fio A1) tells us
 * whether the Icecast mount carries real audio (`live`). "waiting for
 * Spotify..." becomes a SERVER FACT — live 0 while /now says playing on the
 * active device — instead of the receive-went-dry guess; the rx heuristic
 * survives only as the fallback branch for old servers (feature-detected via
 * not_found, CLIENTS.md rules 23-24).
 *
 * Pure: no sockets, no Toolbox, no clock (the loop passes TickCount in), no
 * globals. Strings are UTF-8/ASCII; MacRoman conversion stays at the draw
 * boundary (NOTES.md). Same family as cq_now/cq_guard: host-buildable,
 * host-tested (tests/view_test.c is a scenario table).
 */
#ifndef CQ_VIEW_H
#define CQ_VIEW_H

#include "cq_now.h"

/* Thresholds (ticks are 1/60 s). */
#define CQ_VIEW_ACK_TICKS      480L    /* 8 s ack/settle timeout (b48; the
                                          client half of the server's A2) */
#define CQ_VIEW_STARVE_TICKS   120L    /* ~2 s of "buffering..." after the
                                          interrupt served silence (b45) */
#define CQ_VIEW_DRY_TICKS      180L    /* ~3 s of parked rx = dry mount — the
                                          old-server fallback heuristic only */
#define CQ_VIEW_FACT_STALE_MS  30000LL /* a /stream fact this much older than
                                          the /now snapshot is history, not
                                          fact: fall back to the heuristic */

/* The audio engine's phase, abstracted from the glue's au_state + gPlaying. */
typedef enum {
    CQ_ENGINE_OFF = 0,       /* AU_IDLE: not tuned, the row stays clean */
    CQ_ENGINE_TUNING,        /* AU_HTTP / AU_SYNC: connect + header + sync */
    CQ_ENGINE_BUFFERING,     /* AU_PLAY before the channel starts (prebuffer) */
    CQ_ENGINE_ON_AIR         /* AU_PLAY with the double-buffer engine running */
} cq_engine_phase;

/* A pending transport command awaiting reflection in the snapshot. */
typedef enum {
    CQ_INTENT_NONE = 0,
    CQ_INTENT_SKIP,          /* next/prev: reflected when track_id moves off
                                pre_track_id (none captured -> a playing,
                                identified track reflects) */
    CQ_INTENT_PLAY,          /* play/resume/wake: reflected by state playing */
    CQ_INTENT_PAUSE          /* pause: reflected by state not-playing */
} cq_intent_kind;

/*
 * Everything the render needs, by value — cq_view never calls TickCount and
 * never touches globals. Errors never build one of these: the existing law
 * ("never blank a good snapshot on an error document") is enforced
 * structurally, because an error reply simply doesn't produce a view input.
 */
typedef struct {
    /* the player plane: the current guarded /now snapshot (post cq_guard) */
    const cq_now *snap;        /* NULL before the first adoption */
    unsigned long snap_ticks;  /* TickCount at adoption — the interpolation
                                  anchor (the client-side stand-in for the
                                  server's re-stamped ts) */

    /* the media plane: the /stream fact, when the server has the endpoint */
    int       has_stream_fact; /* 0 = old server / nothing adopted yet */
    int       live;            /* live 0|1 (meaningful iff has_stream_fact) */
    long long fact_ts;         /* server epoch ms of the reading; 0 = unknown */

    /* audio-engine physics */
    cq_engine_phase engine;
    unsigned long ring_fill;     /* decoded PCM backlog, bytes */
    unsigned long prebuf_target; /* bytes before the channel starts (buffer %) */
    unsigned long chunk_bytes;   /* one hardware refill — the audible-tail
                                    floor for "playing out..." (b49) */
    long rx_dry_ticks;           /* ticks since the last stream byte;
                                    -1 = no wire open */
    long starve_ticks;           /* ticks since the interrupt last served
                                    silence; -1 = never this listen */

    /* the pending intent, if any */
    cq_intent_kind intent;
    unsigned long  intent_ticks; /* TickCount when it was issued */
    char           pre_track_id[24]; /* track_id at issue; "" = none (base62
                                        ids are 22 chars) */
    cq_play_state  pre_state;    /* state at issue */

    unsigned long now_ticks;     /* TickCount, passed in by the loop */
} cq_view_in;

/* The entire renderable truth. Fixed buffers so the output is one flat value
 * (diffable with cq_view_differs, no ownership). */
typedef struct {
    char state_word[16];  /* "Playing"/"Paused"/"Stopped" — or the ack text
                             ("Skipping..."), which WINS while pending */
    int  ack_active;      /* state_word is the ack overlay */
    int  title_green;     /* paint the track title in the spot green */
    char status[40];      /* the radio readout, right-aligned; "" = row clean */
    int  buffer_pct;      /* 0..99 while the status is the prebuffer count-up;
                             -1 otherwise */
    int  show_progress;   /* duration known: draw the bar */
    long long position_ms; /* interpolated while playing, clamped */
    long long duration_ms;
    char cover_key[64];   /* album_id to ask the cover cache with; "" = none */
} cq_view;

void cq_view_render(cq_view *out, const cq_view_in *in);

/* Display-significant difference (position compared at 1 s granularity —
 * the finest anything on screen shows). The glue's repaint gate. */
int cq_view_differs(const cq_view *a, const cq_view *b);

#endif /* CQ_VIEW_H */
