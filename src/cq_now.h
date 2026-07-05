/*
 * cq_now — Casquinha Model layer (pure): the /spot/api/1/now snapshot.
 *
 * Ports DeGelato's DGNowSnapshot. An immutable-in-spirit value object built once
 * from a field set. Keys off `state` first; tolerates missing keys and ignores
 * unknown ones (CLIENT-PATTERN.md §2 law 6). Numbers absent -> 0, except
 * `volume`, whose absence is the sentinel -1 ("no device reported a volume").
 *
 * Pure: no sockets, no Toolbox, no clock (the caller passes epoch-ms in).
 */
#ifndef CQ_NOW_H
#define CQ_NOW_H

#include <stddef.h>
#include "cq_codec.h"

typedef enum {
    CQ_STATE_STOPPED = 0,
    CQ_STATE_PLAYING = 1,
    CQ_STATE_PAUSED  = 2
} cq_play_state;

typedef enum {
    CQ_DEV_UNKNOWN = 0,
    CQ_DEV_ACTIVE  = 1,
    CQ_DEV_IDLE    = 2
} cq_dev_state;

typedef struct {
    cq_play_state state;
    char         *track;      /* owned or NULL when absent */
    char         *artist;
    char         *album;
    char         *album_id;
    char         *track_id;
    long long     position_ms;
    long long     duration_ms;
    long long     ts;
    int           volume;     /* -1 when the `volume` key was absent */
    int           queue_len;
    int           api_version;
    cq_dev_state  device;
} cq_now;

/* Build from an already-parsed field set (does not take ownership of f). */
void cq_now_from_fields(cq_now *n, const cq_fields *f);
/* Convenience: parse the raw body, then build. */
void cq_now_from_response(cq_now *n, const unsigned char *data, size_t len);
void cq_now_free(cq_now *n);

int cq_now_has_track(const cq_now *n);      /* track present and non-empty */
int cq_now_has_volume(const cq_now *n);     /* volume >= 0 (not the -1 sentinel) */
int cq_now_device_is_idle(const cq_now *n);

/*
 * Smooth progress between polls. Frozen (returns position_ms unchanged) unless
 * state == playing AND ts > 0; otherwise position_ms + (now_epoch_ms - ts),
 * clamped to [0, duration_ms] (no high clamp when duration_ms is 0/unknown).
 */
long long cq_now_interpolated_position_ms(const cq_now *n, long long now_epoch_ms);

#endif /* CQ_NOW_H */
