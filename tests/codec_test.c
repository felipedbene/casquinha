#include "cq_test.h"
#include "cq_codec.h"

#include <stdlib.h>
#include <string.h>

static const char *get(const char *body, const char *key)
{
    /* helper: parse then look up, leaking into a static field set per call is
     * avoided by using a local set the caller can't free — so we copy out. */
    static char out[256];
    cq_fields f;
    const char *v;
    cq_fields_init(&f);
    cq_fields_parse(&f, (const unsigned char *)body, strlen(body));
    v = cq_fields_get(&f, key);
    if (v) { strncpy(out, v, sizeof(out) - 1); out[sizeof(out) - 1] = '\0'; }
    cq_fields_free(&f);
    return v ? out : NULL;
}

static size_t count(const char *body, size_t len)
{
    cq_fields f;
    size_t c;
    cq_fields_init(&f);
    cq_fields_parse(&f, (const unsigned char *)body, len);
    c = f.count;
    cq_fields_free(&f);
    return c;
}

void codec_tests(void)
{
    printf("codec\n");

    /* basic CRLF */
    CHECK_STR(get("api\t1\r\nstate\tplaying\r\n", "api"), "1", "CRLF api=1");
    CHECK_STR(get("api\t1\r\nstate\tplaying\r\n", "state"), "playing", "CRLF state");

    /* bare LF parses identically */
    CHECK_STR(get("a\t1\nb\t2", "a"), "1", "bare-LF a");
    CHECK_STR(get("a\t1\nb\t2", "b"), "2", "bare-LF b");

    /* only the trailing CR is stripped; inner spaces preserved */
    CHECK_STR(get("k\tName With Spaces\r\n", "k"), "Name With Spaces", "inner spaces kept");

    /* TAB-less lines skipped (garbage + lone dot), real line kept */
    CHECK(count("garbage line\n.\na\t1\n", strlen("garbage line\n.\na\t1\n")) == 1, "skip no-tab lines");
    CHECK_STR(get("garbage line\n.\na\t1\n", "a"), "1", "kept the real line");

    /* last value wins */
    CHECK_STR(get("k\ta\nk\tb\n", "k"), "b", "last wins");

    /* only first TAB splits; later TABs stay in value */
    CHECK_STR(get("k\ta\tb", "k"), "a\tb", "value may contain tab");

    /* empty and NULL -> zero fields */
    CHECK(count("", 0) == 0, "empty -> 0 fields");
    { cq_fields f; cq_fields_init(&f); cq_fields_parse(&f, NULL, 0);
      CHECK(f.count == 0, "NULL -> 0 fields"); cq_fields_free(&f); }

    /* empty key (leading TAB) dropped */
    CHECK(count("\tvalue\n", strlen("\tvalue\n")) == 0, "empty key dropped");

    /* data_is_jpeg */
    { unsigned char j[5] = {0xFF,0xD8,0xFF,0xE0,0x00};
      CHECK(cq_data_is_jpeg(j, 5), "FF D8 -> jpeg"); }
    CHECK(!cq_data_is_jpeg((const unsigned char *)"api\t1", 5), "text -> not jpeg");
    CHECK(!cq_data_is_jpeg((const unsigned char *)"", 0), "empty -> not jpeg");
    CHECK(!cq_data_is_jpeg(NULL, 0), "NULL -> not jpeg");

    if (cq_have_fixtures()) {
        size_t len = 0;
        unsigned char *jpg = cq_fixture("cover_sample.jpg", &len);
        unsigned char *err = cq_fixture("cover_error.txt", &len);
        if (jpg) { CHECK(cq_data_is_jpeg(jpg, len), "fixture cover is JPEG"); free(jpg); }
        if (err) {
            cq_fields f; cq_fields_init(&f);
            CHECK(!cq_data_is_jpeg(err, len), "fixture cover_error is not JPEG");
            cq_fields_parse(&f, err, len);
            CHECK_STR(cq_fields_get(&f, "error"), "bad_range", "cover_error error=bad_range");
            cq_fields_free(&f); free(err);
        }
    }
}
