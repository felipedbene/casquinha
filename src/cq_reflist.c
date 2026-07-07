#include "cq_reflist.h"

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

static char *field_for(const cq_fields *f, const char *prefix, size_t i, const char *suffix)
{
    char key[80];
    snprintf(key, sizeof(key), "%s.%lu.%s", prefix, (unsigned long)i, suffix);
    return (char *)cq_fields_get(f, key);   /* borrowed pointer into f */
}

void cq_ref_list_from_fields(cq_ref_list *l, const cq_fields *f, const char *prefix)
{
    size_t i = 0;
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;

    for (;;) {
        const char *id = field_for(f, prefix, i, "id");
        cq_ref_item *it;
        if (!id) break;   /* first gap ends the list */

        if (l->count == l->cap) {
            size_t ncap = l->cap ? l->cap * 2 : 8;
            cq_ref_item *ni = (cq_ref_item *)realloc(l->items, ncap * sizeof(cq_ref_item));
            if (!ni) break;
            l->items = ni;
            l->cap   = ncap;
        }
        it = &l->items[l->count];
        it->id   = dup_or_null(id);
        it->name = dup_or_null(field_for(f, prefix, i, "name"));
        l->count++;
        i++;
    }
}

void cq_ref_list_from_response(cq_ref_list *l, const unsigned char *data, size_t len,
                               const char *prefix)
{
    cq_fields f;
    cq_fields_init(&f);
    cq_fields_parse(&f, data, len);
    cq_ref_list_from_fields(l, &f, prefix);
    cq_fields_free(&f);
}

void cq_ref_list_free(cq_ref_list *l)
{
    size_t i;
    if (!l) return;
    for (i = 0; i < l->count; i++) {
        free(l->items[i].id);
        free(l->items[i].name);
    }
    free(l->items);
    l->items = NULL;
    l->count = 0;
    l->cap   = 0;
}
