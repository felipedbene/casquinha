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
