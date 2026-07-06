/*
 * cq_cache — Casquinha cover memory (pure): a fixed-slot, FIFO, string-keyed
 * cache of opaque payloads.
 *
 * Fio A (backend-exhaustion audit / CLIENTS.md checklist 6): a client must
 * request a cover AT MOST ONCE per distinct album_id per app run. That needs
 * two things a lone "last album" string can't give:
 *   - a NEGATIVE memory: an album whose fetch FAILED is still "tried" — before
 *     this, a failing /cover was refetched every loop pass (a retry storm that
 *     was exhausting the backend);
 *   - more than one slot: alternating albums A→B→A must not refetch A.
 * Both live here as one structure: an entry present with a NULL value means
 * "tried, no image"; a non-NULL value is the caller's decoded payload (the
 * OS 9 app stores a GWorldPtr; the tests store plain pointers).
 *
 * Ownership: the cache never frees a value — it can't (GWorlds need
 * DisposeGWorld, not free). Every operation that displaces a value RETURNS it
 * for the caller to dispose. Pure — no clock, no allocation, fixed capacity.
 */
#ifndef CQ_CACHE_H
#define CQ_CACHE_H

#define CQ_CACHE_SLOTS   8
#define CQ_CACHE_KEYMAX 64   /* keys longer than this compare truncated */

typedef struct {
    char  keys[CQ_CACHE_SLOTS][CQ_CACHE_KEYMAX];
    void *vals[CQ_CACHE_SLOTS];
    int   used;
} cq_cache;

void cq_cache_init(cq_cache *c);

int cq_cache_count(const cq_cache *c);

/* Has this key been stored (even with a NULL value)? The "already tried"
 * test — distinct from get(), which can't tell NULL-value from absent. */
int cq_cache_has(const cq_cache *c, const char *key);

/* The stored value, or NULL when absent OR stored-as-NULL (see has()). */
void *cq_cache_get(const cq_cache *c, const char *key);

/* Insert, or replace on a key hit. Returns the DISPLACED value — the evicted
 * oldest entry's when full, or the replaced one on a key hit — for the caller
 * to dispose; NULL when nothing non-NULL was displaced. */
void *cq_cache_put(cq_cache *c, const char *key, void *val);

/* Remove the oldest entry and return its value (caller disposes). Returns
 * NULL when empty — but also when the oldest value WAS NULL, so drive drain
 * loops with cq_cache_count(), not the return value. */
void *cq_cache_take_oldest(cq_cache *c);

#endif /* CQ_CACHE_H */
