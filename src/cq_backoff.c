#include "cq_backoff.h"

void cq_backoff_init(cq_backoff *b, long base, long cap)
{
    if (base < 1) base = 1;
    if (cap < base) cap = base;
    b->base    = base;
    b->cap     = cap;
    b->current = base;
}

long cq_backoff_interval(const cq_backoff *b)
{
    return b->current;
}

long cq_backoff_interval_seeded(const cq_backoff *b, unsigned long seed)
{
    long span = b->current / 4;
    unsigned long mix;
    if (span <= 0) return b->current;
    mix = seed * 2654435761UL + 40503UL;   /* multiplicative mix; ticks alone are too regular */
    return b->current + (long)(mix % (unsigned long)(span + 1));
}

void cq_backoff_ok(cq_backoff *b)
{
    b->current = b->base;
}

void cq_backoff_fail(cq_backoff *b)
{
    long next = b->current * 2;
    if (next > b->cap) next = b->cap;
    if (next < b->current) next = b->cap;   /* overflow guard */
    b->current = next;
}
