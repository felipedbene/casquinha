#include "cq_codec.h"

#include <stdlib.h>
#include <string.h>

void cq_fields_init(cq_fields *f)
{
    f->items = NULL;
    f->count = 0;
    f->cap   = 0;
}

void cq_fields_free(cq_fields *f)
{
    size_t i;
    if (!f) return;
    for (i = 0; i < f->count; i++) {
        free(f->items[i].key);
        free(f->items[i].value);
    }
    free(f->items);
    f->items = NULL;
    f->count = 0;
    f->cap   = 0;
}

/* Copy a byte range into a fresh NUL-terminated C string. An embedded NUL
 * truncates the C view (keys/values on the real wire are NUL-free UTF-8; only a
 * garbage response carries a NUL, and there truncation is harmless — the line
 * just becomes an unread junk key). Never returns NULL. */
static char *dup_range(const unsigned char *p, size_t n)
{
    char *s = (char *)malloc(n + 1);
    if (!s) return NULL;
    if (n) memcpy(s, p, n);
    s[n] = '\0';
    return s;
}

static void fields_put(cq_fields *f, const unsigned char *k, size_t klen,
                       const unsigned char *v, size_t vlen)
{
    size_t i;
    char *kc, *vc;

    /* Last-wins: if the key already exists (compare the leading klen bytes and
     * require the stored key to be exactly that length), replace its value. */
    for (i = 0; i < f->count; i++) {
        const char *ek = f->items[i].key;
        if (strlen(ek) == klen && memcmp(ek, k, klen) == 0) {
            vc = dup_range(v, vlen);
            if (!vc) return;
            free(f->items[i].value);
            f->items[i].value = vc;
            return;
        }
    }

    if (f->count == f->cap) {
        size_t ncap = f->cap ? f->cap * 2 : 8;
        cq_field *ni = (cq_field *)realloc(f->items, ncap * sizeof(cq_field));
        if (!ni) return;
        f->items = ni;
        f->cap   = ncap;
    }
    kc = dup_range(k, klen);
    vc = dup_range(v, vlen);
    if (!kc || !vc) { free(kc); free(vc); return; }
    f->items[f->count].key   = kc;
    f->items[f->count].value = vc;
    f->count++;
}

void cq_fields_parse(cq_fields *f, const unsigned char *data, size_t len)
{
    size_t start = 0;
    if (!f || !data || len == 0) return;

    while (start <= len) {
        size_t end, i, line_len;
        int found_tab;
        size_t tab = 0;

        /* line = [start, end) where end is the next '\n' or the buffer end. */
        end = start;
        while (end < len && data[end] != '\n') end++;

        line_len = end - start;
        /* strip exactly one trailing '\r' */
        if (line_len > 0 && data[start + line_len - 1] == '\r') line_len--;

        /* first TAB splits key/value */
        found_tab = 0;
        for (i = 0; i < line_len; i++) {
            if (data[start + i] == '\t') { tab = i; found_tab = 1; break; }
        }
        if (found_tab && tab > 0) {
            fields_put(f,
                       data + start, tab,
                       data + start + tab + 1, line_len - tab - 1);
        }

        if (end >= len) break;   /* consumed the final (possibly LF-less) line */
        start = end + 1;
    }
}

const char *cq_fields_get(const cq_fields *f, const char *key)
{
    size_t i;
    if (!f || !key) return NULL;
    for (i = 0; i < f->count; i++) {
        if (strcmp(f->items[i].key, key) == 0) return f->items[i].value;
    }
    return NULL;
}

int cq_data_is_jpeg(const unsigned char *data, size_t len)
{
    return data && len >= 2 && data[0] == 0xFF && data[1] == 0xD8;
}

long long cq_parse_ll(const char *s)
{
    long long v = 0;
    int neg = 0;
    if (!s) return 0;
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' ||
           *s == '\v' || *s == '\f') s++;
    if (*s == '+' || *s == '-') { neg = (*s == '-'); s++; }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (*s - '0');
        s++;
    }
    return neg ? -v : v;
}
