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

    /* seeded jitter: always in [current, current + current/4], deterministic,
     * and actually varying across seeds */
    { cq_backoff b; unsigned long s; int varied = 0; long first;
      cq_backoff_init(&b, 120, 1800);
      first = cq_backoff_interval_seeded(&b, 0);
      for (s = 0; s < 200; s++) {
          long j = cq_backoff_interval_seeded(&b, s);
          if (j < 120 || j > 150) { CHECK(0, "jitter out of [120,150]"); break; }
          if (j != first) varied = 1;
      }
      CHECK(varied, "jitter varies across seeds");
      CHECK(cq_backoff_interval_seeded(&b, 42) == cq_backoff_interval_seeded(&b, 42),
            "equal seeds give equal jitter"); }

    /* jitter never drops below the un-jittered interval (law 5) */
    { cq_backoff b; unsigned long s;
      cq_backoff_init(&b, 120, 1800);
      cq_backoff_fail(&b);   /* current = 240 */
      for (s = 0; s < 50; s++)
          if (cq_backoff_interval_seeded(&b, s) < 240) { CHECK(0, "jitter went below current"); break; }
      CHECK(1, "jitter stayed >= current after backoff"); }

    /* tiny intervals (span 0) pass through un-jittered */
    { cq_backoff b; cq_backoff_init(&b, 3, 10);
      CHECK(cq_backoff_interval_seeded(&b, 7) == 3, "span 0 -> no jitter"); }
}
