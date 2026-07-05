#include "cq_pls.h"

#include <stdlib.h>
#include <string.h>

static int is_ws(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

static char lc(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive prefix test. */
static int starts_ci(const char *s, const char *pfx)
{
    size_t i;
    for (i = 0; pfx[i]; i++) {
        if (lc(s[i]) != lc(pfx[i])) return 0;
    }
    return 1;
}

/* All chars are ASCII digits. Empty string -> 1 (a `File=` with no index). */
static int all_digits(const char *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] < '0' || s[i] > '9') return 0;
    }
    return 1;
}

static char *dup_str(const char *s)
{
    size_t n = strlen(s);
    char *o = (char *)malloc(n + 1);
    if (!o) return NULL;
    memcpy(o, s, n + 1);
    return o;
}

char *cq_pls_first_url(const char *text)
{
    char *best_pls = NULL;
    long  best_idx = 0;      /* valid only when best_pls != NULL */
    char *first_bare = NULL;
    const char *p;
    char line[1024];

    if (!text) return NULL;

    p = text;
    for (;;) {
        const char *nl = p;
        size_t raw, a, b, len;

        /* one line = up to the next CR or LF */
        while (*nl && *nl != '\r' && *nl != '\n') nl++;
        raw = (size_t)(nl - p);

        /* trim leading/trailing whitespace into a bounded buffer */
        a = 0;
        while (a < raw && is_ws(p[a])) a++;
        b = raw;
        while (b > a && is_ws(p[b - 1])) b--;
        len = b - a;
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, p + a, len);
        line[len] = '\0';

        if (len > 0) {
            if (starts_ci(line, "file")) {
                char *eq = strchr(line, '=');
                if (eq && (size_t)(eq - line) >= 4) {
                    const char *keynum = line + 4;
                    size_t knlen = (size_t)(eq - line) - 4;
                    if (all_digits(keynum, knlen)) {
                        long idx = (knlen == 0) ? 1 : strtol(keynum, NULL, 10);
                        const char *val = eq + 1;
                        if (idx == 0) idx = 1;   /* File0= -> treat as index 1 */
                        while (*val && is_ws(*val)) val++;   /* trim value front */
                        if (*val != '\0' && (!best_pls || idx < best_idx)) {
                            char *nv = dup_str(val);
                            if (nv) { free(best_pls); best_pls = nv; best_idx = idx; }
                        }
                        goto next_line;   /* consumed as a File entry */
                    }
                }
                /* a "file"-prefixed line that isn't File<n>= falls through */
            }
            if (line[0] != '#') {
                if (starts_ci(line, "http://") || starts_ci(line, "https://")) {
                    if (!first_bare) first_bare = dup_str(line);
                }
            }
        }

    next_line:
        if (*nl == '\0') break;
        p = nl + 1;   /* skip the single CR or LF; a CRLF just yields one empty line */
    }

    if (best_pls) { free(first_bare); return best_pls; }
    return first_bare;   /* may be NULL */
}
