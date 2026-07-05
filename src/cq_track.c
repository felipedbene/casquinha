#include "cq_track.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static char *dup_or_null(const char *s)
{
    size_t n;
    char *o;
    if (!s) return NULL;
    n = strlen(s);
    o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n + 1);
    return o;
}

static char *field_for(const cq_fields *f, size_t i, const char *suffix)
{
    char key[64];
    /* item.<i>.<suffix> — matches DGTrackItem's "item.%lu.<suffix>" */
    snprintf(key, sizeof(key), "item.%lu.%s", (unsigned long)i, suffix);
    return (char *)cq_fields_get(f, key);   /* borrowed pointer into f */
}

void cq_track_list_from_fields(cq_track_list *l, const cq_fields *f)
{
    size_t i = 0;
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;

    for (;;) {
        const char *uri = field_for(f, i, "uri");
        cq_track_item *it;
        if (!uri) break;   /* first gap ends the list */

        if (l->count == l->cap) {
            size_t ncap = l->cap ? l->cap * 2 : 8;
            cq_track_item *ni = (cq_track_item *)realloc(l->items, ncap * sizeof(cq_track_item));
            if (!ni) break;
            l->items = ni;
            l->cap   = ncap;
        }
        it = &l->items[l->count];
        it->uri         = dup_or_null(uri);
        it->track       = dup_or_null(field_for(f, i, "track"));
        it->artist      = dup_or_null(field_for(f, i, "artist"));
        it->album_id    = dup_or_null(field_for(f, i, "album_id"));
        it->duration_ms = cq_parse_ll(field_for(f, i, "duration_ms"));
        l->count++;
        i++;
    }
}

void cq_track_list_from_response(cq_track_list *l, const unsigned char *data, size_t len)
{
    cq_fields f;
    cq_fields_init(&f);
    cq_fields_parse(&f, data, len);
    cq_track_list_from_fields(l, &f);
    cq_fields_free(&f);
}

void cq_track_list_free(cq_track_list *l)
{
    size_t i;
    if (!l) return;
    for (i = 0; i < l->count; i++) {
        free(l->items[i].uri);
        free(l->items[i].track);
        free(l->items[i].artist);
        free(l->items[i].album_id);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;
}
