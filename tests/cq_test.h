/* cq_test — a tiny assert-based runner for the pure core (host build). */
#ifndef CQ_TEST_H
#define CQ_TEST_H

#include <stdio.h>
#include <string.h>
#include <stddef.h>

extern int cq_checks;
extern int cq_fails;

#define CHECK(cond, msg) do {                                        \
        cq_checks++;                                                 \
        if (!(cond)) { cq_fails++;                                   \
            printf("  [FAIL] %s  (%s:%d)\n", (msg), __FILE__, __LINE__); } \
    } while (0)

#define CHECK_STR(a, b, msg) CHECK((a) && (b) && strcmp((a),(b)) == 0, msg)
#define CHECK_NULL(a, msg)   CHECK((a) == NULL, msg)

/* Load a fixture by name from $CQ_FIXTURES. Returns a malloc'd buffer and sets
 * *len; returns NULL (and *len = 0) when CQ_FIXTURES is unset or the file is
 * missing, so fixture-backed checks self-skip off the harness. */
unsigned char *cq_fixture(const char *name, size_t *len);
int cq_have_fixtures(void);

void codec_tests(void);
void now_tests(void);
void track_tests(void);
void guard_tests(void);
void view_tests(void);
void debounce_tests(void);
void pls_tests(void);
void mp3_tests(void);
void mp3dec_tests(void);
void decring_tests(void);
void backoff_tests(void);
void cache_tests(void);
void transport_tests(void);

#endif /* CQ_TEST_H */
