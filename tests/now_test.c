#include "cq_test.h"
#include "cq_now.h"

#include <stdlib.h>
#include <string.h>

static void from(cq_now *n, const char *body)
{
    cq_now_from_response(n, (const unsigned char *)body, strlen(body));
}

void now_tests(void)
{
    printf("now\n");

    /* playing snapshot with a 64-bit ts, device active */
    {
        cq_now n;
        from(&n,
            "api\t1\r\nstate\tplaying\r\ntrack\tConstru\xC3\xA7\xC3\xA3o\r\n"
            "artist\tChico Buarque\r\nalbum_id\tALB\r\nposition_ms\t26221\r\n"
            "duration_ms\t383626\r\ndevice\tactive\r\nvolume\t100\r\n"
            "ts\t1783105644431\r\n");
        CHECK(n.state == CQ_STATE_PLAYING, "state playing");
        CHECK(n.api_version == 1, "api 1");
        CHECK(cq_now_has_track(&n), "has track");
        CHECK(cq_now_has_volume(&n), "has volume");
        CHECK(!cq_now_device_is_idle(&n), "device active not idle");
        CHECK(n.ts == 1783105644431LL, "64-bit ts parsed");
        CHECK(n.volume == 100, "volume 100");
        CHECK_STR(n.artist, "Chico Buarque", "artist");
        cq_now_free(&n);
    }

    /* stopped: no track, no volume (-1 sentinel) */
    {
        cq_now n;
        from(&n, "api\t1\r\nstate\tstopped\r\n");
        CHECK(n.state == CQ_STATE_STOPPED, "state stopped");
        CHECK(!cq_now_has_track(&n), "stopped has no track");
        CHECK(!cq_now_has_volume(&n), "stopped has no volume");
        CHECK(n.volume == -1, "volume -1 sentinel");
        cq_now_free(&n);
    }

    /* paused */
    { cq_now n; from(&n, "state\tpaused\n");
      CHECK(n.state == CQ_STATE_PAUSED, "paused"); cq_now_free(&n); }

    /* device idle */
    { cq_now n; from(&n, "state\tplaying\ndevice\tidle\n");
      CHECK(cq_now_device_is_idle(&n), "device idle"); cq_now_free(&n); }

    /* device unknown when absent */
    { cq_now n; from(&n, "state\tplaying\n");
      CHECK(n.device == CQ_DEV_UNKNOWN, "device unknown when absent");
      CHECK(!cq_now_device_is_idle(&n), "unknown not idle"); cq_now_free(&n); }

    /* unknown keys ignored, missing tolerated (law 6) */
    {
        cq_now n;
        from(&n, "api\t1\nstate\tplaying\ntrack\tX\ncover_url\thttp://x\nfuture_field\t42\nvolume\t80\n");
        CHECK_STR(n.track, "X", "known key recovered past unknown ones");
        CHECK(n.volume == 80, "volume past unknown keys");
        cq_now_free(&n);
    }

    /* truncated before the tab: trailing "track" (no TAB) is dropped */
    { cq_now n; from(&n, "state\tplaying\ntrack");
      CHECK(n.state == CQ_STATE_PLAYING, "state survives truncation");
      CHECK(n.track == NULL, "fragment before tab dropped"); cq_now_free(&n); }

    /* truncated inside the value: partial value kept */
    { cq_now n; from(&n, "state\tplaying\ntrack\tHalf A Titl");
      CHECK_STR(n.track, "Half A Titl", "partial value kept"); cq_now_free(&n); }

    /* interpolation while playing */
    { cq_now n; from(&n, "state\tplaying\nposition_ms\t1000\nts\t5000\n");
      CHECK(cq_now_interpolated_position_ms(&n, 6500) == 2500, "interp playing = 2500");
      cq_now_free(&n); }

    /* interpolation clamped to duration */
    { cq_now n; from(&n, "state\tplaying\nposition_ms\t1000\nduration_ms\t1200\nts\t5000\n");
      CHECK(cq_now_interpolated_position_ms(&n, 99999) == 1200, "interp clamped to duration");
      cq_now_free(&n); }

    /* interpolation frozen when paused */
    { cq_now n; from(&n, "state\tpaused\nposition_ms\t1000\nts\t5000\n");
      CHECK(cq_now_interpolated_position_ms(&n, 99999) == 1000, "interp frozen when paused");
      cq_now_free(&n); }

    if (cq_have_fixtures()) {
        size_t len = 0;
        unsigned char *b;

        b = cq_fixture("now_live.txt", &len);
        if (b) { cq_now n; cq_now_from_response(&n, b, len);
                 CHECK(n.api_version == 1, "now_live api 1");
                 CHECK(n.state == CQ_STATE_PLAYING, "now_live playing");
                 cq_now_free(&n); free(b); }

        b = cq_fixture("now_accents.txt", &len);
        if (b) { cq_now n; cq_now_from_response(&n, b, len);
                 CHECK_STR(n.track, "Constru\xC3\xA7\xC3\xA3o", "now_accents track Construcao (UTF-8)");
                 CHECK_STR(n.artist, "Chico Buarque", "now_accents artist");
                 cq_now_free(&n); free(b); }

        b = cq_fixture("now_unknown_keys.txt", &len);
        if (b) { cq_now n; cq_now_from_response(&n, b, len);
                 CHECK_STR(n.track, "X", "now_unknown_keys track X");
                 CHECK(n.volume == 80, "now_unknown_keys volume 80");
                 cq_now_free(&n); free(b); }

        /* the degenerate fixtures must never crash */
        { const char *names[3]; int i;
          names[0] = "now_empty.txt"; names[1] = "now_garbage.txt"; names[2] = "now_truncated.txt";
          for (i = 0; i < 3; i++) {
              b = cq_fixture(names[i], &len);
              if (b) { cq_now n; cq_now_from_response(&n, b, len);
                       CHECK(1, names[i]); cq_now_free(&n); free(b); }
          }
        }
    }
}
