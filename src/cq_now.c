#include "cq_now.h"

#include <stdlib.h>
#include <string.h>

/* strdup is POSIX, not C89; keep our own so the pure core builds under Retro68. */
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

/* longLongValue-equivalent number parse lives in cq_codec (cq_parse_ll). */
#define parse_ll cq_parse_ll

static cq_play_state state_from(const char *s)
{
    if (s && strcmp(s, "playing") == 0) return CQ_STATE_PLAYING;
    if (s && strcmp(s, "paused")  == 0) return CQ_STATE_PAUSED;
    return CQ_STATE_STOPPED;   /* incl. NULL, "stopped", unknown */
}

static cq_dev_state device_from(const char *s)
{
    if (s && strcmp(s, "active") == 0) return CQ_DEV_ACTIVE;
    if (s && strcmp(s, "idle")   == 0) return CQ_DEV_IDLE;
    return CQ_DEV_UNKNOWN;      /* incl. NULL/absent */
}

void cq_now_from_fields(cq_now *n, const cq_fields *f)
{
    const char *vol;
    memset(n, 0, sizeof(*n));

    n->state       = state_from(cq_fields_get(f, "state"));
    n->device      = device_from(cq_fields_get(f, "device"));
    n->track       = dup_or_null(cq_fields_get(f, "track"));
    n->artist      = dup_or_null(cq_fields_get(f, "artist"));
    n->album       = dup_or_null(cq_fields_get(f, "album"));
    n->album_id    = dup_or_null(cq_fields_get(f, "album_id"));
    n->track_id    = dup_or_null(cq_fields_get(f, "track_id"));
    n->position_ms = parse_ll(cq_fields_get(f, "position_ms"));
    n->duration_ms = parse_ll(cq_fields_get(f, "duration_ms"));
    n->ts          = parse_ll(cq_fields_get(f, "ts"));
    n->queue_len   = (int)parse_ll(cq_fields_get(f, "queue_len"));
    n->api_version = (int)parse_ll(cq_fields_get(f, "api"));

    vol = cq_fields_get(f, "volume");
    n->volume = vol ? (int)parse_ll(vol) : -1;   /* -1 sentinel when absent */
}

void cq_now_from_response(cq_now *n, const unsigned char *data, size_t len)
{
    cq_fields f;
    cq_fields_init(&f);
    cq_fields_parse(&f, data, len);
    cq_now_from_fields(n, &f);
    cq_fields_free(&f);
}

void cq_now_free(cq_now *n)
{
    if (!n) return;
    free(n->track);
    free(n->artist);
    free(n->album);
    free(n->album_id);
    free(n->track_id);
    n->track = n->artist = n->album = n->album_id = n->track_id = NULL;
}

int cq_now_has_track(const cq_now *n)
{
    return n->track != NULL && n->track[0] != '\0';
}

int cq_now_has_volume(const cq_now *n)
{
    return n->volume >= 0;
}

int cq_now_device_is_idle(const cq_now *n)
{
    return n->device == CQ_DEV_IDLE;
}

long long cq_now_interpolated_position_ms(const cq_now *n, long long now_epoch_ms)
{
    long long est;
    if (n->state != CQ_STATE_PLAYING || n->ts <= 0) return n->position_ms;
    est = n->position_ms + (now_epoch_ms - n->ts);
    if (est < 0) est = 0;
    if (n->duration_ms > 0 && est > n->duration_ms) est = n->duration_ms;
    return est;
}
