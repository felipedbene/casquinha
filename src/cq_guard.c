#include "cq_guard.h"

void cq_guard_init(cq_guard *g)
{
    g->last_ts = 0;
}

int cq_guard_accept_ts(cq_guard *g, long long ts)
{
    if (ts <= 0) return 1;                    /* unknown ts never blocks, never moves mark */
    if (g->last_ts > 0 && ts < g->last_ts) return 0;  /* regressed: staler replica */
    g->last_ts = ts;                          /* advance (or re-adopt equal ts) */
    return 1;
}

void cq_guard_reset(cq_guard *g)
{
    g->last_ts = 0;
}
