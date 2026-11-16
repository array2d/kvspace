#define _GNU_SOURCE 
#include "kvspace_shm.h"
#include "test_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "test_public_io.h"
#define CHECK(x) \
    do { \
        if (!(x)) { \
            fprintf(stderr, "failed: %s (line %d)\n", #x, __LINE__); \
            return 1; \
        } \
    } while (0)
static uint8_t *slack(const char *s, uint64_t *n) {
    uint8_t *v = NULL;
    if (kvspace_test_wire_slack((const uint8_t *)s, strlen(s), "", &v, n) != 0)
        return NULL;
    return v;
}
int main(void) {
    char dir[] = "/tmp/kvspace-art-align-XXXXXX";
    CHECK(mkdtemp(dir) != NULL);
    char path[128];
    snprintf(path, sizeof path, "%s/db", dir);
    kvspace_t *kv = t_open(path, 8UL * 64 * 64 * 64 * 64);
    CHECK(kv != NULL);
    for (int i = 0; i < 64; i++) {
        char key[64];
        snprintf(key, sizeof key, "/a/%0*d", 1 + i % 7, i);
        uint64_t n = 0;
        uint8_t *v = slack("x", &n);
        CHECK(v != NULL);
        CHECK(t_set_wire(kv, key, v, (int32_t)n) == 0);
        free(v);
    }
    CHECK(kvspaceShmArtAlignCheck(kv) == -1);
    printf("PASS art_alignment\n");
    return 0;
}
