#include "cq_test.h"
#include "cq_pls.h"

#include <stdlib.h>
#include <string.h>

static int has_suffix(const char *s, const char *suf)
{
    size_t ls, lf;
    if (!s) return 0;
    ls = strlen(s); lf = strlen(suf);
    return ls >= lf && strcmp(s + ls - lf, suf) == 0;
}

void pls_tests(void)
{
    printf("pls\n");

    /* basic PLS */
    { char *u = cq_pls_first_url(
        "[playlist]\nNumberOfEntries=1\nFile1=http://192.0.2.30:8000/spotify.mp3\nVersion=2\n");
      CHECK_STR(u, "http://192.0.2.30:8000/spotify.mp3", "PLS File1 extracted");
      free(u); }

    /* lowest File index wins regardless of order */
    { char *u = cq_pls_first_url("File2=http://b/2.mp3\nFile1=http://a/1.mp3\n");
      CHECK_STR(u, "http://a/1.mp3", "lowest File index wins");
      free(u); }

    /* M3U bare URL, directives skipped */
    { char *u = cq_pls_first_url("#EXTM3U\n#EXTINF:-1,x\nhttp://host:8000/spotify.mp3\n");
      CHECK_STR(u, "http://host:8000/spotify.mp3", "M3U bare URL");
      free(u); }

    /* no URL -> NULL */
    { CHECK_NULL(cq_pls_first_url("[playlist]\nNumberOfEntries=0\n"), "no URL -> NULL");
      CHECK_NULL(cq_pls_first_url(""), "empty -> NULL");
      CHECK_NULL(cq_pls_first_url(NULL), "NULL -> NULL"); }

    /* a File entry beats a bare URL even when the bare line came first */
    { char *u = cq_pls_first_url("http://bare/first.mp3\nFile1=http://file/win.mp3\n");
      CHECK_STR(u, "http://file/win.mp3", "PLS File beats bare URL");
      free(u); }

    if (cq_have_fixtures()) {
        size_t len = 0;
        unsigned char *b = cq_fixture("stream.pls", &len);
        if (b) {
            char *u = cq_pls_first_url((const char *)b);
            CHECK(u && strncmp(u, "http://", 7) == 0, "stream.pls url has http:// prefix");
            CHECK(has_suffix(u, "/spotify.mp3"), "stream.pls url ends /spotify.mp3");
            free(u); free(b);
        }
    }
}
