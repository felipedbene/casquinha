/*
 * cq_reflist — Casquinha Model layer (pure): an {id,name} reference list.
 *
 * The lighter sibling of cq_track: search's artist.<i>.{id,name} and
 * album.<i>.{id,name} blocks, and the artist-discography endpoint
 * (/spot/api/1/artist/<id>/albums) whose item.<i>.{id,name} rows share the exact
 * shape. Parametrized by the key PREFIX ("artist" | "album" | "item"): the run
 * ends at the first index whose .id is absent (the *_len header is ignored, the
 * id scan is authoritative — same law as cq_track).
 */
#ifndef CQ_REFLIST_H
#define CQ_REFLIST_H

#include <stddef.h>
#include "cq_codec.h"

typedef struct {
    char *id;    /* owned; Spotify base62 id (required per item) */
    char *name;  /* owned or NULL */
} cq_ref_item;

typedef struct {
    cq_ref_item *items;
    size_t       count;
    size_t       cap;
} cq_ref_list;

/* Scan "<prefix>.<i>.id" / "<prefix>.<i>.name" from a parsed field set. */
void cq_ref_list_from_fields(cq_ref_list *l, const cq_fields *f, const char *prefix);
/* Parse raw bytes then scan (convenience over cq_fields). */
void cq_ref_list_from_response(cq_ref_list *l, const unsigned char *data, size_t len,
                               const char *prefix);
void cq_ref_list_free(cq_ref_list *l);

#endif /* CQ_REFLIST_H */
