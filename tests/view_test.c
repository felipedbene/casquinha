#include "cq_test.h"
#include "cq_view.h"

/*
 * cq_view scenario table (fio B): each case is inputs -> expected status text
 * + state word + ack flag. The snapshots are stack values with literal
 * strings (never freed) — cq_view only reads them.
 */

/* A healthy playing-on-the-active-device snapshot. */
static cq_now snap_playing(void)
{
    cq_now n;
    memset(&n, 0, sizeof(n));
    n.state       = CQ_STATE_PLAYING;
    n.device      = CQ_DEV_ACTIVE;
    n.track       = (char *)"Construcao";
    n.track_id    = (char *)"3AKtKhw5MEd2XvJcPqofNJ";
    n.album_id    = (char *)"1nJvji2KIlWSseXRSlNYsC";
    n.position_ms = 60000;
    n.duration_ms = 180000;
    n.ts          = 1000000;
    return n;
}

/* Baseline input: engine on air, healthy wire, no fact, no intent, t=1000. */
static cq_view_in base_in(const cq_now *snap)
{
    cq_view_in in;
    memset(&in, 0, sizeof(in));
    in.snap          = snap;
    in.snap_ticks    = 1000;
    in.engine        = CQ_ENGINE_ON_AIR;
    in.ring_fill     = 512 * 1024;
    in.prebuf_target = 384 * 1024;
    in.chunk_bytes   = 32 * 1024;
    in.rx_dry_ticks  = 5;
    in.starve_ticks  = -1;
    in.now_ticks     = 1000;
    return in;
}

void view_tests(void)
{
    printf("view\n");

    /* --- cold start: engine off, nothing adopted --- */
    { cq_view_in in = base_in(NULL); cq_view v;
      in.engine = CQ_ENGINE_OFF;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "", "cold start: row clean");
      CHECK_STR(v.state_word, "", "cold start: no state word");
      CHECK(!v.ack_active, "cold start: no ack");
      CHECK(!v.show_progress, "cold start: no progress bar");
      CHECK(v.buffer_pct == -1, "cold start: no buffer pct");
      CHECK_STR(v.cover_key, "", "cold start: no cover key"); }

    /* --- tuning --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.engine = CQ_ENGINE_TUNING;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "tuning in...", "tuning: status");
      CHECK_STR(v.state_word, "Playing", "tuning: state word from snapshot"); }

    /* --- buffering count-up --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.engine = CQ_ENGINE_BUFFERING;
      in.ring_fill = 96 * 1024;                        /* 25% of 384K */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "buffering... 25%", "buffering: live percentage");
      CHECK(v.buffer_pct == 25, "buffering: pct field");
      in.ring_fill = 384 * 1024;                       /* at/over target */
      cq_view_render(&v, &in);
      CHECK(v.buffer_pct == 99, "buffering: pct capped at 99");
      in.ring_fill = 0; in.prebuf_target = 0;          /* degenerate target */
      cq_view_render(&v, &in);
      CHECK(v.buffer_pct == 0, "buffering: zero target never divides"); }

    /* --- on air, healthy (with and without the fact) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "on air: no fact, healthy");
      CHECK_STR(v.state_word, "Playing", "on air: state word");
      CHECK(v.title_green, "on air: title green while playing");
      in.has_stream_fact = 1; in.live = 1; in.fact_ts = s.ts;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "on air: fact live 1 agrees"); }

    /* --- pause: tail draining, then standing by (b49) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      s.state = CQ_STATE_PAUSED;
      in.ring_fill = 128 * 1024;                       /* tail > one chunk */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "playing out...", "paused: tail still audible");
      CHECK_STR(v.state_word, "Paused", "paused: state word");
      CHECK(!v.title_green, "paused: title not green");
      in.ring_fill = 8 * 1024;                         /* tail drained */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "standing by", "paused: tuned, silent transmitter"); }

    /* --- dry-mount anomaly, FACT branch (fio B: a server fact) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.has_stream_fact = 1; in.live = 0; in.fact_ts = s.ts;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "waiting for Spotify...",
                "fact: live 0 + playing + active = the anomaly");
      /* rx healthy is irrelevant — the fact outranks the heuristic */
      in.rx_dry_ticks = 0;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "waiting for Spotify...", "fact: rx state irrelevant");
      /* live 0 while paused = expected silence, not the anomaly */
      s.state = CQ_STATE_PAUSED; in.ring_fill = 0;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "standing by", "fact: live 0 paused is expected");
      /* live 0 while playing ELSEWHERE (device idle): expected silence too */
      s.state = CQ_STATE_PLAYING; s.device = CQ_DEV_IDLE;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "standing by", "fact: live 0 idle-device is expected");
      /* live 1 quiets a dry-rx false alarm (client stall, not the mount) */
      s.device = CQ_DEV_ACTIVE; in.live = 1; in.rx_dry_ticks = 500;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "fact: live 1 overrides the rx heuristic"); }

    /* --- dry-mount anomaly during prebuffer (fact only) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.engine = CQ_ENGINE_BUFFERING; in.ring_fill = 0;
      in.has_stream_fact = 1; in.live = 0; in.fact_ts = s.ts;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "waiting for Spotify...",
                "buffering: fact anomaly outranks a count that will never move");
      CHECK(v.buffer_pct == -1, "buffering: no pct under the anomaly");
      /* the heuristic must NOT do this — dry rx IS what buffering looks like */
      in.has_stream_fact = 0; in.rx_dry_ticks = 500;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "buffering... 0%", "buffering: heuristic stays quiet"); }

    /* --- dry-mount anomaly, heuristic branch (old server fallback) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.rx_dry_ticks = 200;                           /* > 3 s parked */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "waiting for Spotify...", "heuristic: rx dry 200");
      in.rx_dry_ticks = 100;                           /* not dry yet */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "heuristic: rx dry 100 is fine");
      in.rx_dry_ticks = -1;                            /* no wire open */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "heuristic: no wire, no verdict");
      s.state = CQ_STATE_PAUSED; in.rx_dry_ticks = 500; in.ring_fill = 0;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "standing by", "heuristic: dry rx while paused is expected"); }

    /* --- stale fact falls back to the heuristic --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.has_stream_fact = 1; in.live = 0;
      in.fact_ts = s.ts - CQ_VIEW_FACT_STALE_MS - 1;   /* >30 s behind /now */
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "stale fact: live 0 from history ignored");
      in.rx_dry_ticks = 200;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "waiting for Spotify...",
                "stale fact: the heuristic takes over"); }

    /* --- starvation tell (b45) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.starve_ticks = 10;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "buffering...", "starve: within the 2 s window");
      in.starve_ticks = CQ_VIEW_STARVE_TICKS;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "starve: window closed");
      in.starve_ticks = -1;
      cq_view_render(&v, &in);
      CHECK_STR(v.status, "on air", "starve: never starved"); }

    /* --- skip ack lifecycle: issued -> pending -> reflected --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.intent = CQ_INTENT_SKIP;
      in.intent_ticks = 900;
      strcpy(in.pre_track_id, "3AKtKhw5MEd2XvJcPqofNJ");   /* == snap: pending */
      in.pre_state = CQ_STATE_PLAYING;
      cq_view_render(&v, &in);
      CHECK(v.ack_active, "skip: pending while track unchanged");
      CHECK_STR(v.state_word, "Skipping...", "skip: ack wins the state word");
      s.track_id = (char *)"0999999999999999999999";      /* the flip lands */
      cq_view_render(&v, &in);
      CHECK(!v.ack_active, "skip: reflected clears the ack");
      CHECK_STR(v.state_word, "Playing", "skip: state word returns"); }

    /* --- skip ack: issued -> timeout --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.intent = CQ_INTENT_SKIP;
      in.intent_ticks = 900;
      strcpy(in.pre_track_id, "3AKtKhw5MEd2XvJcPqofNJ");
      in.now_ticks = 900 + CQ_VIEW_ACK_TICKS;              /* 8 s later */
      cq_view_render(&v, &in);
      CHECK(!v.ack_active, "skip: 8 s timeout clears the ack");
      CHECK_STR(v.state_word, "Playing", "skip: snapshot word after timeout"); }

    /* --- stale-snapshot interplay: guard-dropped polls never reach the view,
     *     so a pending ack simply stays pending (input unchanged) --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.intent = CQ_INTENT_SKIP;
      in.intent_ticks = 900;
      strcpy(in.pre_track_id, "3AKtKhw5MEd2XvJcPqofNJ");
      in.now_ticks = 900 + 300;                            /* 5 s in, no flip */
      cq_view_render(&v, &in);
      CHECK(v.ack_active, "skip: still pending on the same snapshot");
      CHECK_STR(v.state_word, "Skipping...", "skip: ack holds through stale polls"); }

    /* --- skip with no pre id (from stopped): a playing track reflects --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      s.state = CQ_STATE_STOPPED; s.track_id = NULL;
      in.intent = CQ_INTENT_SKIP; in.intent_ticks = 990;
      in.pre_state = CQ_STATE_STOPPED;
      cq_view_render(&v, &in);
      CHECK(v.ack_active, "skip/no-pre: pending while stopped");
      s = snap_playing();                                   /* play/from lands */
      cq_view_render(&v, &in);
      CHECK(!v.ack_active, "skip/no-pre: playing identified track reflects"); }

    /* --- play + pause intents suppress the contradictory word --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      s.state = CQ_STATE_PAUSED; in.ring_fill = 0;
      in.intent = CQ_INTENT_PLAY; in.intent_ticks = 990;
      in.pre_state = CQ_STATE_PAUSED;
      cq_view_render(&v, &in);
      CHECK(v.ack_active, "play: pending while still paused");
      CHECK_STR(v.state_word, "Starting...", "play: ack text");
      s.state = CQ_STATE_PLAYING;
      cq_view_render(&v, &in);
      CHECK(!v.ack_active, "play: reflected by state playing");
      in.intent = CQ_INTENT_PAUSE; in.pre_state = CQ_STATE_PLAYING;
      cq_view_render(&v, &in);
      CHECK(v.ack_active, "pause: pending while still playing");
      CHECK_STR(v.state_word, "Pausing...", "pause: ack text");
      s.state = CQ_STATE_PAUSED;
      cq_view_render(&v, &in);
      CHECK(!v.ack_active, "pause: reflected by state not-playing");
      CHECK_STR(v.state_word, "Paused", "pause: word after reflection"); }

    /* --- no snapshot yet: an intent still acks (never crashes) --- */
    { cq_view_in in = base_in(NULL); cq_view v;
      in.intent = CQ_INTENT_PLAY; in.intent_ticks = 990;
      cq_view_render(&v, &in);
      CHECK(v.ack_active, "no snap: intent pending");
      CHECK_STR(v.state_word, "Starting...", "no snap: ack text stands alone"); }

    /* --- interpolated progress --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      in.snap_ticks = 1000; in.now_ticks = 1120;            /* 2 s later */
      cq_view_render(&v, &in);
      CHECK(v.position_ms == 62000, "progress: +2 s while playing");
      CHECK(v.show_progress, "progress: bar shown with duration");
      CHECK(v.duration_ms == 180000, "progress: duration through");
      in.now_ticks = 1000 + 60 * 100000;                    /* way past the end */
      cq_view_render(&v, &in);
      CHECK(v.position_ms == 180000, "progress: clamped at duration");
      s.state = CQ_STATE_PAUSED;
      in.now_ticks = 1120;
      cq_view_render(&v, &in);
      CHECK(v.position_ms == 60000, "progress: frozen while paused");
      s.state = CQ_STATE_PLAYING; s.duration_ms = 0;
      cq_view_render(&v, &in);
      CHECK(!v.show_progress, "progress: no bar without duration");
      CHECK(v.position_ms == 62000, "progress: no high clamp without duration"); }

    /* --- cover key --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view v;
      cq_view_render(&v, &in);
      CHECK_STR(v.cover_key, "1nJvji2KIlWSseXRSlNYsC", "cover: album_id copied");
      s.album_id = NULL;
      cq_view_render(&v, &in);
      CHECK_STR(v.cover_key, "", "cover: no album, empty key"); }

    /* --- the repaint gate --- */
    { cq_now s = snap_playing(); cq_view_in in = base_in(&s); cq_view a, b;
      cq_view_render(&a, &in);
      in.now_ticks += 30;                                   /* +0.5 s */
      cq_view_render(&b, &in);
      CHECK(!cq_view_differs(&a, &b), "differ: sub-second motion is no repaint");
      in.now_ticks += 60;                                   /* crosses a second */
      cq_view_render(&b, &in);
      CHECK(cq_view_differs(&a, &b), "differ: a new second repaints");
      in.starve_ticks = 5;                                  /* status flips */
      cq_view_render(&b, &in);
      CHECK(cq_view_differs(&a, &b), "differ: status change repaints"); }
}
