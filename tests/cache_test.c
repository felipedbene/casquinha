#include "cq_test.h"
#include "cq_cache.h"

/* Distinct non-NULL payloads: the cache stores opaque pointers, so any stable
 * addresses do (the app stores GWorldPtrs). */
static int pay[12];

void cache_tests(void)
{
    printf("cache\n");

    /* empty */
    { cq_cache c; cq_cache_init(&c);
      CHECK(cq_cache_count(&c) == 0, "starts empty");
      CHECK(!cq_cache_has(&c, "a"), "miss on empty");
      CHECK_NULL(cq_cache_get(&c, "a"), "get on empty is NULL");
      CHECK_NULL(cq_cache_take_oldest(&c), "take on empty is NULL"); }

    /* basic put/get — the canonical UTF-8 key survives byte-exact */
    { cq_cache c; cq_cache_init(&c);
      CHECK_NULL(cq_cache_put(&c, "Constru\xc3\xa7\xc3\xa3o", &pay[0]), "first put displaces nothing");
      CHECK(cq_cache_count(&c) == 1, "count 1");
      CHECK(cq_cache_has(&c, "Constru\xc3\xa7\xc3\xa3o"), "has Construção");
      CHECK(cq_cache_get(&c, "Constru\xc3\xa7\xc3\xa3o") == &pay[0], "get Construção");
      CHECK(!cq_cache_has(&c, "Construcao"), "ASCII-folded key is a different key"); }

    /* negative entry: tried-but-failed is present with a NULL value */
    { cq_cache c; cq_cache_init(&c);
      cq_cache_put(&c, "failed-album", (void *)0);
      CHECK(cq_cache_has(&c, "failed-album"), "NULL-valued key still counts as tried");
      CHECK_NULL(cq_cache_get(&c, "failed-album"), "and yields no payload"); }

    /* replace on key hit returns the old value, keeps count */
    { cq_cache c; cq_cache_init(&c);
      cq_cache_put(&c, "a", &pay[0]);
      CHECK(cq_cache_put(&c, "a", &pay[1]) == &pay[0], "replace returns old value");
      CHECK(cq_cache_count(&c) == 1, "replace keeps count");
      CHECK(cq_cache_get(&c, "a") == &pay[1], "new value readable"); }

    /* FIFO eviction: filling past capacity displaces the oldest */
    { cq_cache c; int i; char k[8];
      cq_cache_init(&c);
      for (i = 0; i < CQ_CACHE_SLOTS; i++) {
          snprintf(k, sizeof(k), "k%d", i);
          CHECK_NULL(cq_cache_put(&c, k, &pay[i]), "no eviction while filling");
      }
      CHECK(cq_cache_count(&c) == CQ_CACHE_SLOTS, "full");
      CHECK(cq_cache_put(&c, "extra", &pay[8]) == &pay[0], "9th put evicts oldest value");
      CHECK(cq_cache_count(&c) == CQ_CACHE_SLOTS, "still full");
      CHECK(!cq_cache_has(&c, "k0"), "evicted key gone");
      CHECK(cq_cache_has(&c, "k1"), "next-oldest survives");
      CHECK(cq_cache_has(&c, "extra"), "newcomer present"); }

    /* drain via take_oldest, in insertion order, count-driven (NULL values
     * make the return value useless as a loop condition) */
    { cq_cache c; cq_cache_init(&c);
      cq_cache_put(&c, "one", &pay[0]);
      cq_cache_put(&c, "two", (void *)0);
      cq_cache_put(&c, "three", &pay[2]);
      CHECK(cq_cache_take_oldest(&c) == &pay[0], "oldest out first");
      CHECK_NULL(cq_cache_take_oldest(&c), "NULL-valued entry drains as NULL");
      CHECK(cq_cache_count(&c) == 1, "one left after two takes");
      CHECK(cq_cache_take_oldest(&c) == &pay[2], "last one out");
      CHECK(cq_cache_count(&c) == 0, "drained"); }

    /* oversized keys compare truncated — consistently on put and lookup */
    { cq_cache c; char big[CQ_CACHE_KEYMAX + 16]; int i;
      cq_cache_init(&c);
      for (i = 0; i < (int)sizeof(big) - 1; i++) big[i] = 'x';
      big[sizeof(big) - 1] = '\0';
      cq_cache_put(&c, big, &pay[3]);
      CHECK(cq_cache_has(&c, big), "oversized key found again");
      CHECK(cq_cache_get(&c, big) == &pay[3], "oversized key yields its value"); }
}
