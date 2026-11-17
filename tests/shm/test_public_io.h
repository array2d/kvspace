#ifndef KVSPACE_TEST_PUBLIC_IO_H
#define KVSPACE_TEST_PUBLIC_IO_H 
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void *t_open(const char *path, size_t data_size) {
#ifdef RECOVERY_TEST_HOOKS
    kvspaceTestSetDataSize(data_size);
#else
    (void)data_size;
#endif
    char dsn[1200];
    snprintf(dsn, sizeof dsn, "shm://%s", path);
    return kvspaceConnect(dsn);
}
static void t_close(void *kv) {
    kvspaceClose(kv);
}
static int t_set_wire(void *kv, const char *key, const uint8_t *wire,
                      int32_t len) {
    if (!wire || len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t body = kvspaceXvalueHeadLen(head);
    uint64_t attrs = kvspaceXvalueHeadAttrsLen(head);
    uint64_t off = 0;
    char err[128] = {0};
    if (kvspaceRealloc(kv, key, body, attrs, &off, err, sizeof err) != 0)
        return -1;
    uint8_t *base = kvspaceXvalueInProcessAddress(kv);
    if (!base)
        return -1;
    memcpy(base + off + KVSPACE_XVALUE_HEAD_LEN,
           wire + KVSPACE_XVALUE_HEAD_LEN, body);
    if (attrs)
        memcpy(base + off + KVSPACE_XVALUE_HEAD_LEN + body,
               wire + KVSPACE_XVALUE_HEAD_LEN + body, attrs);
    return 0;
}
static uint8_t *t_get_wire_depth(void *kv, const char *key, int resolve,
                                 int32_t *out_len, int depth) {
    uint64_t off = 0;
    char err[128] = {0};
    if (kvspaceResolveOffset(kv, key, NULL, &off, err,
                             sizeof err) != 0)
        return NULL;
    uint8_t *base = kvspaceXvalueInProcessAddress(kv);
    if (!base)
        return NULL;
    uint8_t *wire = base + off;
    const kvspaceXvalueHead *head = (const kvspaceXvalueHead *)(const void *)wire;
    uint64_t total = kvspaceXvalueHeadTotalLen(head);
    if (total < KVSPACE_XVALUE_HEAD_LEN || depth > 8)
        return NULL;
    if (resolve) {
        const uint8_t *lt = NULL;
        uint16_t lt_len = 0;
        if (kvspaceXvalueGetAttr(wire, total, 1, &lt, &lt_len) == 0 &&
            lt_len > 0 && lt[0] == '*') {
            char target[512];
            uint64_t body = kvspaceXvalueHeadLen(head);
            if (body >= sizeof target)
                return NULL;
            memcpy(target, wire + KVSPACE_XVALUE_HEAD_LEN, (size_t)body);
            target[body] = '\0';
            return t_get_wire_depth(kv, target, 1, out_len, depth + 1);
        }
    }
    if (out_len)
        *out_len = (int32_t)total;
    return wire;
}
static uint8_t *t_get_wire(void *kv, const char *key, int resolve,
                          int32_t *out_len) {
    return t_get_wire_depth(kv, key, resolve, out_len, 0);
}
static int t_del(void *kv, const char *key) {
    char err[128] = {0};
    return kvspaceFree(kv, key, err, sizeof err) == KVSPACE_OK ? 0 : -1;
}
static int t_deltree(void *kv, const char *key) {
    char err[128] = {0};
    return kvspaceDeltree(kv, key, err, sizeof err) == KVSPACE_OK ? 0 : -1;
}
static int t_cp(void *kv, const char *src, const char *dst) {
    char err[128] = {0};
    return kvspaceCp(kv, src, dst, err, sizeof err) == KVSPACE_OK ? 0 : -1;
}
static int t_cptree(void *kv, const char *src, const char *dst) {
    char err[128] = {0};
    return kvspaceCpdir(kv, src, dst, err, sizeof err) == KVSPACE_OK ? 0 : -1;
}
static int t_mkindex(void *kv, const char *key) {
    char err[128] = {0};
    return kvspaceMkindex(kv, key, err, sizeof err) == KVSPACE_OK ? 0 : -1;
}
static int t_realloc(void *kv, const char *key, uint64_t body_len,
                     uint64_t attr_len, uint64_t *out_offset) {
    char err[128] = {0};
    return kvspaceRealloc(kv, key, body_len, attr_len, out_offset, err,
                          sizeof err) == KVSPACE_OK ? 0 : -1;
}
static uint8_t *t_data_base(void *kv) {
    return kvspaceXvalueInProcessAddress(kv);
}
static int t_list(void *kv, const char *prefix, bool expand_ext, int resolve,
                  char ***out_names, int32_t *out_count) {
    (void)expand_ext;
    (void)resolve;
    uint32_t length = 0, count = 0;
    uint64_t generation = 0;
    char err[128] = {0};
    int status = kvspaceList(kv, prefix, NULL, 0, &length, &count, &generation,
                             err, sizeof err);
    if (status == KVSPACE_MISSING || status == KVSPACE_NOT_INDEX) {
        *out_names = NULL;
        *out_count = 0;
        return 0;
    }
    if (status != KVSPACE_OK)
        return -1;
    char *buffer = malloc(length ? length : 1);
    if (!buffer)
        return -1;
    if (kvspaceList(kv, prefix, buffer, length, &length, &count, &generation,
                    err, sizeof err) != KVSPACE_OK) {
        free(buffer);
        return -1;
    }
    char **names = calloc(count ? count : 1, sizeof(char *));
    if (!names) {
        free(buffer);
        return -1;
    }
    const char *p = buffer;
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; i++) {
        size_t len = strlen(p);
        int replaced = 0;
        for (uint32_t j = 0; j < kept; j++) {
            size_t other = strlen(names[j]);
            if (other == len + 1 && strncmp(names[j], p, len) == 0 &&
                names[j][len] == '/') {
                replaced = 1;
                break;
            }
            if (len == other + 1 && strncmp(p, names[j], other) == 0 &&
                p[other] == '/') {
                free(names[j]);
                names[j] = strdup(p);
                replaced = 1;
                break;
            }
        }
        if (!replaced)
            names[kept++] = strdup(p);
        p += len + 1;
    }
    free(buffer);
    *out_names = names;
    *out_count = (int32_t)kept;
    return 0;
}
#endif
