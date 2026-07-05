#include "cq_test.h"
#include "cq_debounce.h"

#include <stdlib.h>

void debounce_tests(void)
{
    printf("debounce\n");

    /* empty holds nothing */
    { cq_debounce d; cq_debounce_init(&d);
      CHECK(!cq_debounce_has(&d), "empty has nothing");
      CHECK_NULL(cq_debounce_take(&d), "empty take -> NULL");
      cq_debounce_free(&d); }

    /* last tap wins */
    { cq_debounce d; char *v; cq_debounce_init(&d);
      cq_debounce_set(&d, "/spot/api/1/prev");
      cq_debounce_set(&d, "/spot/api/1/next");
      cq_debounce_set(&d, "/spot/api/1/prev");
      v = cq_debounce_take(&d);
      CHECK_STR(v, "/spot/api/1/prev", "last tap wins");
      free(v);
      cq_debounce_free(&d); }

    /* take clears; no double-send */
    { cq_debounce d; char *v; cq_debounce_init(&d);
      cq_debounce_set(&d, "/spot/api/1/next");
      v = cq_debounce_take(&d);
      CHECK_STR(v, "/spot/api/1/next", "take returns value");
      free(v);
      CHECK(!cq_debounce_has(&d), "cleared after take");
      CHECK_NULL(cq_debounce_take(&d), "second take -> NULL (no double-send)");
      cq_debounce_free(&d); }

    /* re-arm after a flush behaves independently */
    { cq_debounce d; char *v; cq_debounce_init(&d);
      cq_debounce_set(&d, "a"); free(cq_debounce_take(&d));
      cq_debounce_set(&d, "b");
      v = cq_debounce_take(&d);
      CHECK_STR(v, "b", "re-arm after take");
      free(v);
      cq_debounce_free(&d); }
}
