#include "cq_test.h"
#include "cq_reflist.h"

#include <stdlib.h>
#include <string.h>

static void from(cq_ref_list *l, const char *body, const char *prefix)
{
    cq_ref_list_from_response(l, (const unsigned char *)body, strlen(body), prefix);
}

void reflist_tests(void)
{
    printf("reflist\n");

    /* search artist block: artist.<i>.{id,name} */
    {
        cq_ref_list l;
        from(&l,
            "api\t1\nresult_len\t1\n"
            "item.0.uri\tspotify:track:AAA\n"           /* a track row is ignored */
            "artist_len\t2\n"
            "artist.0.id\tART0\nartist.0.name\tChico Buarque\n"
            "artist.1.id\tART1\nartist.1.name\tMPB4\n", "artist");
        CHECK(l.count == 2, "two artists");
        CHECK_STR(l.items[0].id, "ART0", "artist0 id");
        CHECK_STR(l.items[0].name, "Chico Buarque", "artist0 name");
        CHECK_STR(l.items[1].id, "ART1", "artist1 id");
        cq_ref_list_free(&l);
    }

    /* same doc, album prefix -> only the album block */
    {
        cq_ref_list l;
        from(&l,
            "album_len\t1\nalbum.0.id\tALB0\nalbum.0.name\tConstru\xC3\xA7\xC3\xA3o\n"
            "artist.0.id\tART0\nartist.0.name\tChico\n", "album");
        CHECK(l.count == 1, "one album (artist rows ignored)");
        CHECK_STR(l.items[0].id, "ALB0", "album0 id");
        cq_ref_list_free(&l);
    }

    /* artist-albums endpoint: item.<i>.{id,name} */
    {
        cq_ref_list l;
        from(&l,
            "api\t1\nresult_len\t2\ntotal\t30\noffset\t0\n"
            "item.0.id\tALB0\nitem.0.name\tConstru\xC3\xA7\xC3\xA3o\n"
            "item.1.id\tALB1\nitem.1.name\tChico 50 Anos\n", "item");
        CHECK(l.count == 2, "two albums via item prefix");
        CHECK_STR(l.items[1].id, "ALB1", "item1 id");
        cq_ref_list_free(&l);
    }

    /* a name-less id still counts (name -> NULL, not a crash) */
    {
        cq_ref_list l;
        from(&l, "artist.0.id\tART0\n", "artist");
        CHECK(l.count == 1, "id without name still an item");
        CHECK(l.items[0].name == NULL, "missing name -> NULL");
        cq_ref_list_free(&l);
    }

    /* empty / gap: a missing index ends the run */
    {
        cq_ref_list l;
        from(&l, "artist.0.id\tA\nartist.2.id\tC\n", "artist");
        CHECK(l.count == 1, "stops at first gap");
        cq_ref_list_free(&l);
    }
    { cq_ref_list l; from(&l, "api\t1\n", "album");
      CHECK(l.count == 0, "no album block -> 0 items"); cq_ref_list_free(&l); }
}
