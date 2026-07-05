/*
 * cq_guard — Casquinha Reconciler (pure): the monotonic ts-guard.
 *
 * Ports DeGelato's DGSnapshotGuard. CLIENT-PATTERN.md §2 law 2 / DeGelato R3:
 * gopher-spot runs two load-balanced replicas, each micro-caching /now ~1s, so
 * consecutive polls can return `ts` OUT OF ORDER. Adopt a snapshot only if its
 * ts has not regressed. THIS GUARD IS MANDATORY — without it the UI flip-flops
 * (track rewinds, seek knob jumps). Reset on reconnect so a backend clock-reset
 * can't lock adoption out forever.
 */
#ifndef CQ_GUARD_H
#define CQ_GUARD_H

typedef struct {
    long long last_ts;   /* high-water mark; 0 = none adopted yet */
} cq_guard;

void cq_guard_init(cq_guard *g);

/*
 * Returns 1 to adopt, 0 to drop:
 *   - ts <= 0 (unknown/absent): always adopt, and NEVER move the mark;
 *   - ts < mark (a staler replica): drop;
 *   - otherwise adopt and set mark = ts (equal ts is idempotent, still adopted).
 */
int cq_guard_accept_ts(cq_guard *g, long long ts);

void cq_guard_reset(cq_guard *g);

#endif /* CQ_GUARD_H */
