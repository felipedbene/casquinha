#include "cq_test.h"
#include "cq_backoff.h"

void backoff_tests(void)
{
    printf("backoff\n");

    /* starts at base */
    { cq_backoff b; cq_backoff_init(&b, 120, 1800);
      CHECK(cq_backoff_interval(&b) == 120, "starts at base"); }

    /* fail doubles, capped */
    { cq_backoff b; cq_backoff_init(&b, 120, 1800);
      cq_backoff_fail(&b); CHECK(cq_backoff_interval(&b) == 240, "fail -> 240");
      cq_backoff_fail(&b); CHECK(cq_backoff_interval(&b) == 480, "fail -> 480");
      cq_backoff_fail(&b); CHECK(cq_backoff_interval(&b) == 960, "fail -> 960");
      cq_backoff_fail(&b); CHECK(cq_backoff_interval(&b) == 1800, "fail -> cap 1800");
      cq_backoff_fail(&b); CHECK(cq_backoff_interval(&b) == 1800, "stays at cap"); }

    /* ok resets to base */
    { cq_backoff b; cq_backoff_init(&b, 120, 1800);
      cq_backoff_fail(&b); cq_backoff_fail(&b);
      CHECK(cq_backoff_interval(&b) == 480, "backed off to 480");
      cq_backoff_ok(&b);
      CHECK(cq_backoff_interval(&b) == 120, "ok resets to base");
      cq_backoff_ok(&b);
      CHECK(cq_backoff_interval(&b) == 120, "repeated ok stays at base"); }

    /* degenerate inputs clamp sanely */
    { cq_backoff b; cq_backoff_init(&b, 0, 0);
      CHECK(cq_backoff_interval(&b) == 1, "base<1 clamps to 1");
      cq_backoff_fail(&b); CHECK(cq_backoff_interval(&b) == 1, "cap<base clamps to base"); }
}
