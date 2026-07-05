/*
 * cq_codec — Casquinha Codec layer (pure).
 *
 * Ports DeGelato's DGApiParser: raw response bytes -> a {key:value} field set,
 * plus the one binary-endpoint sniff. Length-aware (the wire can carry embedded
 * NULs in a garbage/truncated response and must never crash), keeps every value
 * byte verbatim (UTF-8 on the wire; MacRoman conversion happens only at the
 * QuickDraw draw boundary, never here — see NOTES.md).
 *
 * CLIENT-PATTERN.md §1 Codec. Pure: no sockets, no Toolbox, no clock.
 */
#ifndef CQ_CODEC_H
#define CQ_CODEC_H

#include <stddef.h>

/* One parsed "key<TAB>value" line. Both strings are NUL-terminated, owned. */
typedef struct {
    char *key;
    char *value;
} cq_field;

/* A field set. Last value wins for a repeated key (stored in place). */
typedef struct {
    cq_field *items;
    size_t    count;
    size_t    cap;
} cq_fields;

void cq_fields_init(cq_fields *f);
void cq_fields_free(cq_fields *f);

/*
 * Parse a key<TAB>value document, appending to f. Tolerant tokenizer:
 *   - split into lines on '\n' (LF); a final line with no trailing LF is a line;
 *   - strip exactly ONE trailing '\r' per line (so CRLF and bare-LF parse alike);
 *   - split key/value on the FIRST '\t' only (later TABs stay in the value);
 *   - a line with no TAB is skipped (blank lines, the lone "." terminator, junk);
 *   - a line whose key is empty (leading TAB) is dropped;
 *   - last value wins for a repeated key.
 * Never fails; a NULL/empty buffer yields no fields.
 */
void cq_fields_parse(cq_fields *f, const unsigned char *data, size_t len);

/* Last stored value for key, or NULL if absent. */
const char *cq_fields_get(const cq_fields *f, const char *key);

/*
 * Binary sniff for the one binary endpoint (/cover): CLIENT-PATTERN.md §2 law 7.
 * True iff the first two bytes are the JPEG SOI marker 0xFF 0xD8. Anything
 * shorter (including empty/NULL) is false — a cover error is a tab-KV text doc.
 */
int cq_data_is_jpeg(const unsigned char *data, size_t len);

/*
 * Parse a signed 64-bit integer the way NSString -longLongValue does: skip
 * leading whitespace, take an optional sign, then decimal digits; stop at the
 * first non-digit; NULL/empty/no-digits -> 0. Hand-rolled so the pure core does
 * not depend on libc strtoll being present on the OS 9 target.
 */
long long cq_parse_ll(const char *s);

#endif /* CQ_CODEC_H */
