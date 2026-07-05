#include "cq_debounce.h"

#include <stdlib.h>
#include <string.h>

static char *dup_str(const char *s)
{
    size_t n;
    char *o;
    if (!s) return NULL;
    n = strlen(s);
    o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n + 1);
    return o;
}

void cq_debounce_init(cq_debounce *d)
{
    d->pending = NULL;
}

void cq_debounce_set(cq_debounce *d, const char *value)
{
    char *nv = dup_str(value);   /* NULL when value is NULL -> clears */
    free(d->pending);
    d->pending = nv;
}

int cq_debounce_has(const cq_debounce *d)
{
    return d->pending != NULL;
}

char *cq_debounce_take(cq_debounce *d)
{
    char *v = d->pending;
    d->pending = NULL;
    return v;   /* ownership transfers to caller */
}

void cq_debounce_free(cq_debounce *d)
{
    free(d->pending);
    d->pending = NULL;
}
