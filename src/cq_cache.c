#include "cq_cache.h"

#include <string.h>

/* Slot 0 is always the oldest; insertion appends at used-1 after any shift. */

static void key_copy(char *dst, const char *src)
{
    strncpy(dst, src ? src : "", CQ_CACHE_KEYMAX - 1);
    dst[CQ_CACHE_KEYMAX - 1] = '\0';
}

static int key_find(const cq_cache *c, const char *key)
{
    char k[CQ_CACHE_KEYMAX];
    int i;
    key_copy(k, key);                 /* compare truncated like we store */
    for (i = 0; i < c->used; i++)
        if (strcmp(c->keys[i], k) == 0) return i;
    return -1;
}

void cq_cache_init(cq_cache *c)
{
    memset(c, 0, sizeof(*c));
}

int cq_cache_count(const cq_cache *c)
{
    return c->used;
}

int cq_cache_has(const cq_cache *c, const char *key)
{
    return key_find(c, key) >= 0;
}

void *cq_cache_get(const cq_cache *c, const char *key)
{
    int i = key_find(c, key);
    return i >= 0 ? c->vals[i] : (void *)0;
}

static void *shift_out_oldest(cq_cache *c)
{
    void *out = c->vals[0];
    int i;
    for (i = 1; i < c->used; i++) {
        memcpy(c->keys[i - 1], c->keys[i], CQ_CACHE_KEYMAX);
        c->vals[i - 1] = c->vals[i];
    }
    c->used--;
    return out;
}

void *cq_cache_put(cq_cache *c, const char *key, void *val)
{
    void *displaced = (void *)0;
    int i = key_find(c, key);

    if (i >= 0) {                     /* key hit: replace in place, keep age */
        displaced = c->vals[i];
        c->vals[i] = val;
        return displaced;
    }
    if (c->used == CQ_CACHE_SLOTS)
        displaced = shift_out_oldest(c);
    key_copy(c->keys[c->used], key);
    c->vals[c->used] = val;
    c->used++;
    return displaced;
}

void *cq_cache_take_oldest(cq_cache *c)
{
    if (c->used == 0) return (void *)0;
    return shift_out_oldest(c);
}
