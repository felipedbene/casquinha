#include "cq_test.h"

#include <stdlib.h>

int cq_checks = 0;
int cq_fails  = 0;

int cq_have_fixtures(void)
{
    const char *base = getenv("CQ_FIXTURES");
    return base && base[0];
}

unsigned char *cq_fixture(const char *name, size_t *len)
{
    const char *base = getenv("CQ_FIXTURES");
    char path[1024];
    FILE *fp;
    long sz;
    unsigned char *buf;
    size_t got;

    if (len) *len = 0;
    if (!base || !base[0]) return NULL;

    snprintf(path, sizeof(path), "%s/%s", base, name);
    fp = fopen(path, "rb");
    if (!fp) return NULL;

    fseek(fp, 0, SEEK_END);
    sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }

    buf = (unsigned char *)malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[got] = '\0';
    if (len) *len = got;
    return buf;
}
