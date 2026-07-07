#include "cq_view.h"

#include <stdio.h>
#include <string.h>

/*
 * Has the snapshot caught up with the pending intent? The client half of the
 * server's A2 settle predicate: pure, state-vs-intent, no clock (the timeout
 * lives in the caller of this file's render, as CQ_VIEW_ACK_TICKS).
 */
static int intent_reflected(const cq_view_in *in)
{
    const cq_now *s = in->snap;
    if (!s) return 0;
    switch (in->intent) {
    case CQ_INTENT_SKIP:
        if (in->pre_track_id[0])
            return s->track_id != NULL &&
                   strcmp(s->track_id, in->pre_track_id) != 0;
        /* no pre id captured (skip from stopped/unknown): any identified,
         * playing track is the skip having landed */
        return s->track_id != NULL && s->state == CQ_STATE_PLAYING;
    case CQ_INTENT_PLAY:
        return s->state == CQ_STATE_PLAYING;
    case CQ_INTENT_PAUSE:
        return s->state != CQ_STATE_PLAYING;
    default:
        return 1;
    }
}

/* The /stream fact is usable when present and contemporaneous with the /now
 * snapshot (both ts are the SAME server's epoch-ms clock). A fact that has
 * fallen >30 s behind an advancing snapshot is history — heuristic branch. */
static int fact_usable(const cq_view_in *in)
{
    if (!in->has_stream_fact) return 0;
    if (in->snap && in->snap->ts > 0 && in->fact_ts > 0 &&
        in->snap->ts - in->fact_ts > CQ_VIEW_FACT_STALE_MS)
        return 0;
    return 1;
}

static int playing_active(const cq_view_in *in)
{
    return in->snap != NULL &&
           in->snap->state == CQ_STATE_PLAYING &&
           in->snap->device == CQ_DEV_ACTIVE;
}

/*
 * The genuine anomaly — someone believes we're playing but no audio reaches
 * the mount. Fact branch (CLIENTS.md rule 23): live 0 while /now says playing
 * on the active device; everything else about live 0 is EXPECTED silence.
 * Heuristic branch (old server, rule 24): rx parked >3 s while playing —
 * exactly the b45 tell, computed here instead of in ServiceAudio.
 */
static int mount_anomaly(const cq_view_in *in)
{
    if (fact_usable(in))
        return in->live == 0 && playing_active(in);
    return in->snap != NULL && in->snap->state == CQ_STATE_PLAYING &&
           in->rx_dry_ticks >= 0 && in->rx_dry_ticks >= CQ_VIEW_DRY_TICKS;
}

/* The tuned-but-silent readout (b49): the ring's tail is still audible for a
 * few seconds (radio latency), then we're tuned to a silent transmitter. */
static void tail_status(cq_view *out, const cq_view_in *in)
{
    if (in->ring_fill > in->chunk_bytes)
        strcpy(out->status, "playing out...");
    else
        strcpy(out->status, "standing by");
}

void cq_view_render(cq_view *out, const cq_view_in *in)
{
    const cq_now *s = in->snap;

    memset(out, 0, sizeof(*out));
    out->buffer_pct = -1;

    /* --- state word — or the pending-intent ack, which wins (b48) --------
     * "Paused" flashing mid-skip was the anarchy this fixes: while an intent
     * is pending, the snapshot describes the PAST, so the ack text replaces
     * the contradictory state word until reflection or the 8 s timeout. */
    {
        int pending = in->intent != CQ_INTENT_NONE &&
                      (long)(in->now_ticks - in->intent_ticks) < CQ_VIEW_ACK_TICKS &&
                      !intent_reflected(in);
        if (pending) {
            out->ack_active = 1;
            strcpy(out->state_word,
                   in->intent == CQ_INTENT_SKIP ? "Skipping..." :
                   in->intent == CQ_INTENT_PLAY ? "Starting..." :
                                                  "Pausing...");
        } else if (s) {
            strcpy(out->state_word,
                   s->state == CQ_STATE_PLAYING ? "Playing" :
                   s->state == CQ_STATE_PAUSED  ? "Paused"  : "Stopped");
        }
    }

    out->title_green = (s != NULL && s->state == CQ_STATE_PLAYING);

    /* --- the radio readout (b45/b49) + the media-plane fact (fio B) ------ */
    switch (in->engine) {
    case CQ_ENGINE_OFF:
        break;                              /* the row stays clean */
    case CQ_ENGINE_TUNING:
        strcpy(out->status, "tuning in...");
        break;
    case CQ_ENGINE_BUFFERING:
        /* The prebuffer count-up is THE anti-re-click device — unless the
         * server FACT says the mount is silent while Spotify believes we're
         * playing: then the count will never move and the honest message is
         * that the lever is on the other plane. (The rx heuristic never
         * overrides the count-up: buffering is exactly what dry rx looks
         * like at this phase, so only the fact may speak here.) */
        if (fact_usable(in) && in->live == 0 && playing_active(in)) {
            strcpy(out->status, "waiting for Spotify...");
        } else {
            unsigned long tgt = in->prebuf_target ? in->prebuf_target : 1;
            long pct = (long)(in->ring_fill * 100UL / tgt);
            if (pct > 99) pct = 99;
            out->buffer_pct = (int)pct;
            snprintf(out->status, sizeof(out->status),
                     "buffering... %d%%", out->buffer_pct);
        }
        break;
    case CQ_ENGINE_ON_AIR:
        if (s && s->state != CQ_STATE_PLAYING) {
            tail_status(out, in);           /* upstream paused/stopped (b49) */
        } else if (mount_anomaly(in)) {
            strcpy(out->status, "waiting for Spotify...");
        } else if (fact_usable(in) && in->live == 0) {
            /* live 0 without the playing+active pair: EXPECTED silence
             * (playing elsewhere, device idle) — tuned to a silent
             * transmitter, not an anomaly */
            tail_status(out, in);
        } else if (in->starve_ticks >= 0 &&
                   in->starve_ticks < CQ_VIEW_STARVE_TICKS) {
            strcpy(out->status, "buffering...");   /* starve tell (b45) */
        } else {
            strcpy(out->status, "on air");
        }
        break;
    }

    /* --- progress: interpolated between polls, frozen unless playing ------
     * The anchor is the ADOPTION tick, not the render tick: ticks since
     * adoption is the client's (now - ts), the server's ts being re-stamped
     * at reply time by the A2 settle. The glue keeps the anchor still across
     * equal-ts re-adoptions, so cache-window polls can't rewind the bar. */
    if (s) {
        long long pos = s->position_ms;
        if (s->state == CQ_STATE_PLAYING)
            pos += (long long)(unsigned long)(in->now_ticks - in->snap_ticks)
                   * 1000 / 60;
        if (pos < 0) pos = 0;
        if (s->duration_ms > 0 && pos > s->duration_ms) pos = s->duration_ms;
        out->position_ms   = pos;
        out->duration_ms   = s->duration_ms;
        out->show_progress = s->duration_ms > 0;
        if (s->album_id) {
            strncpy(out->cover_key, s->album_id, sizeof(out->cover_key) - 1);
            out->cover_key[sizeof(out->cover_key) - 1] = '\0';
        }
    }
}

int cq_view_differs(const cq_view *a, const cq_view *b)
{
    return strcmp(a->state_word, b->state_word) != 0 ||
           strcmp(a->status, b->status)         != 0 ||
           strcmp(a->cover_key, b->cover_key)   != 0 ||
           a->ack_active     != b->ack_active     ||
           a->title_green    != b->title_green    ||
           a->buffer_pct     != b->buffer_pct     ||
           a->show_progress  != b->show_progress  ||
           a->duration_ms    != b->duration_ms    ||
           a->position_ms / 1000 != b->position_ms / 1000;
}
