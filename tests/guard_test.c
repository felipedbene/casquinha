#include "cq_test.h"
#include "cq_guard.h"

void guard_tests(void)
{
    printf("guard\n");

    /* in-order adoption */
    { cq_guard g; cq_guard_init(&g);
      CHECK(cq_guard_accept_ts(&g, 100), "100 accepted");
      CHECK(cq_guard_accept_ts(&g, 200), "200 accepted");
      CHECK(cq_guard_accept_ts(&g, 201), "201 accepted"); }

    /* out-of-order rejection */
    { cq_guard g; cq_guard_init(&g);
      cq_guard_accept_ts(&g, 200);
      CHECK(!cq_guard_accept_ts(&g, 150), "150 rejected (regressed)");
      CHECK(!cq_guard_accept_ts(&g, 199), "199 rejected (regressed)");
      CHECK(cq_guard_accept_ts(&g, 200), "200 equal accepted");
      CHECK(cq_guard_accept_ts(&g, 201), "201 accepted"); }

    /* equal ts does not advance past itself */
    { cq_guard g; cq_guard_init(&g);
      CHECK(cq_guard_accept_ts(&g, 500), "500 accepted");
      CHECK(cq_guard_accept_ts(&g, 500), "500 again accepted");
      CHECK(!cq_guard_accept_ts(&g, 499), "499 still rejected (mark stays 500)"); }

    /* unknown ts (<=0) never blocks and never moves the mark */
    { cq_guard g; cq_guard_init(&g);
      CHECK(cq_guard_accept_ts(&g, 0), "ts 0 accepted");
      CHECK(cq_guard_accept_ts(&g, 100), "100 sets mark");
      CHECK(cq_guard_accept_ts(&g, 0), "ts 0 accepted, no mark move");
      CHECK(cq_guard_accept_ts(&g, -5), "ts -5 accepted, no mark move");
      CHECK(!cq_guard_accept_ts(&g, 50), "50 rejected (mark still 100)"); }

    /* reset path (backend clock reset) */
    { cq_guard g; cq_guard_init(&g);
      cq_guard_accept_ts(&g, 500);
      CHECK(!cq_guard_accept_ts(&g, 400), "400 rejected before reset");
      cq_guard_reset(&g);
      CHECK(cq_guard_accept_ts(&g, 400), "400 accepted after reset");
      CHECK(!cq_guard_accept_ts(&g, 399), "399 rejected (new mark 400)"); }
}
