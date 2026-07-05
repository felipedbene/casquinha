/*
 * cq_track — Casquinha Model layer (pure): one item.<i>.* list row.
 *
 * Ports DeGelato's DGTrackItem. One shape serves both /queue and /search (and a
 * playlist's tracks): a contiguous run of item.<i>.{uri,track,artist,album_id,
 * duration_ms} keys. The list ends at the first index whose .uri is absent;
 * result_len / queue_len headers are ignored (the uri scan is authoritative).
 */
#ifndef CQ_TRACK_H
#define CQ_TRACK_H

#include <stddef.h>
#include "cq_codec.h"

typedef struct {
    char     *uri;        /* owned; "spotify:track:<id>" (required per item) */
    char     *track;      /* owned or NULL */
    char     *artist;     /* owned or NULL */
    char     *album_id;   /* owned or NULL */
    long long duration_ms;
} cq_track_item;

typedef struct {
    cq_track_item *items;
    size_t         count;
    size_t         cap;
} cq_track_list;

void cq_track_list_from_fields(cq_track_list *l, const cq_fields *f);
void cq_track_list_from_response(cq_track_list *l, const unsigned char *data, size_t len);
void cq_track_list_free(cq_track_list *l);

#endif /* CQ_TRACK_H */
