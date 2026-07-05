#include "cq_test.h"
#include "cq_track.h"

#include <stdlib.h>
#include <string.h>

static void from(cq_track_list *l, const char *body)
{
    cq_track_list_from_response(l, (const unsigned char *)body, strlen(body));
}

void track_tests(void)
{
    printf("track\n");

    /* basic 2-item list; item.1 has no album_id */
    {
        cq_track_list l;
        from(&l,
            "api\t1\nresult_len\t2\n"
            "item.0.uri\tspotify:track:AAA\nitem.0.track\tConstru\xC3\xA7\xC3\xA3o\n"
            "item.0.artist\tChico\nitem.0.album_id\tALB0\nitem.0.duration_ms\t383626\n"
            "item.1.uri\tspotify:track:BBB\nitem.1.track\tOutra\nitem.1.artist\tAlguem\n"
            "item.1.duration_ms\t1000\n");
        CHECK(l.count == 2, "two items");
        CHECK_STR(l.items[0].uri, "spotify:track:AAA", "item0 uri");
        CHECK_STR(l.items[0].album_id, "ALB0", "item0 album_id");
        CHECK(l.items[0].duration_ms == 383626, "item0 duration");
        CHECK(l.items[1].album_id == NULL, "item1 no album_id -> NULL");
        cq_track_list_free(&l);
    }

    /* empty list -> zero items (not a crash) */
    { cq_track_list l; from(&l, "api\t1\nresult_len\t0\n");
      CHECK(l.count == 0, "empty list -> 0 items"); cq_track_list_free(&l); }

    /* a gap ends the list: item.0 and item.2 present, item.1 missing -> only 1 */
    { cq_track_list l;
      from(&l, "item.0.uri\tspotify:track:A\nitem.2.uri\tspotify:track:C\n");
      CHECK(l.count == 1, "stops at first gap"); cq_track_list_free(&l); }

    if (cq_have_fixtures()) {
        size_t len = 0;
        unsigned char *b;

        b = cq_fixture("search_sample.txt", &len);
        if (b) { cq_track_list l; cq_track_list_from_response(&l, b, len);
                 CHECK(l.count > 0 && l.count <= 10, "search 1..10 items");
                 CHECK(l.count > 0 && strncmp(l.items[0].uri, "spotify:track:", 14) == 0,
                       "search first uri prefix");
                 cq_track_list_free(&l); free(b); }

        b = cq_fixture("queue_sample.txt", &len);
        if (b) { cq_track_list l; cq_track_list_from_response(&l, b, len);
                 CHECK(l.count > 0, "queue has items");
                 CHECK(l.count > 0 && strncmp(l.items[0].uri, "spotify:track:", 14) == 0,
                       "queue first uri prefix");
                 cq_track_list_free(&l); free(b); }
    }
}
