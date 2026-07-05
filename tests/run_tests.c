#include "cq_test.h"

#include <stdlib.h>

/*
 * Casquinha pure-core test runner (host build, fully offline).
 * Mirrors DeGelato's OCUnit suite; fixtures come from $CQ_FIXTURES (self-skips
 * when unset). Exit 0 iff every check passed.
 */
int main(void)
{
    printf("Casquinha pure-core tests%s\n",
           cq_have_fixtures() ? " (with fixtures)" : " (no fixtures — file-backed checks skipped)");
    printf("----------------------------------------\n");

    codec_tests();
    now_tests();
    track_tests();
    guard_tests();
    debounce_tests();
    pls_tests();
    backoff_tests();
    transport_tests();

    printf("----------------------------------------\n");
    printf("%d checks, %d failed\n", cq_checks, cq_fails);
    return cq_fails == 0 ? 0 : 1;
}
