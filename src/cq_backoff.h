/*
 * cq_backoff — Casquinha Reconciler (pure): exponential poll backoff.
 *
 * A well-behaved client must not keep polling at full cadence while the backend
 * is failing — on a Spotify 429 the bridge returns `error upstream`, and hammering
 * it every 2 s only deepens the rate-limit. So on an error we back off (double the
 * interval, capped) and on a healthy reply we snap back to the base cadence.
 * Complementary to law 5 (poll no faster than the micro-cache): base stays 2 s,
 * backoff only ever makes it slower.
 *
 * Unit-agnostic: the caller supplies whatever it measures time in (the OS 9 app
 * uses TickCount ticks; the tests use plain integers). Pure — no clock inside.
 */
#ifndef CQ_BACKOFF_H
#define CQ_BACKOFF_H

typedef struct {
    long base;      /* the healthy interval (e.g. 120 ticks = 2 s) */
    long cap;       /* the longest interval backoff may reach */
    long current;   /* the interval to wait right now */
} cq_backoff;

/* base is the normal cadence; cap bounds the backoff. current starts at base. */
void cq_backoff_init(cq_backoff *b, long base, long cap);

/* The interval the caller should wait before the next poll. */
long cq_backoff_interval(const cq_backoff *b);

/* interval() plus deterministic POSITIVE jitter derived from seed: a value in
 * [current, current + current/4]. Jitter only ever waits LONGER, so the base
 * cadence keeps honoring law 5 (poll no faster than the micro-cache) while
 * multiple clients desynchronize. Pure — the caller supplies the entropy (the
 * OS 9 app passes TickCount); equal seeds give equal results. */
long cq_backoff_interval_seeded(const cq_backoff *b, unsigned long seed);

/* A healthy (error-free) reply: reset to the base cadence. */
void cq_backoff_ok(cq_backoff *b);

/* A failure (transport error or an `error` document): double the interval,
 * clamped to cap. */
void cq_backoff_fail(cq_backoff *b);

#endif /* CQ_BACKOFF_H */
