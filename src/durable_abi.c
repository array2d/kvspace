/* durable_abi.c — 对齐 kvspace-durable 的 C ABI（kvspace_conn 等 22 符号），
 * 让 kvlang 的 Rust layout 零改动对接 kvspace-c 的 SHM。内部复用 kvsc_* 与 xvalue_*。 */

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "kvspace/kvspace.h"
#include "kvspace/xvalue.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

/* 对齐 kvspace-durable 的 KVHead（repr(C)）。kind+ndim+dims 即完整 kindexp。 */
typedef struct {
    uint8_t kind[32];
    uint8_t is_ptr;
    int32_t array_len;
    int32_t body_len;
    int32_t body_offset;
    int32_t ndim;
    int32_t dims[8];
} kvhead_t;

static int parse_shm_path(const char *dsn, char *out, size_t osz) {
    const char *sep = strstr(dsn, "://");
    if (!sep || strncmp(dsn, "shm", (size_t)(sep - dsn)) != 0) return -1;
    snprintf(out, osz, "%s", sep + 3);
    return 0;
}

void *kvspace_conn(const char *dsn) {
    char path[1024];
    if (parse_shm_path(dsn, path, sizeof path) != 0) return NULL;
    return (void *)kvsc_open(path, SHM_DEFAULT_SIZE);
}

void kvspace_free(void *h) {
    if (h) kvsc_close((kvspace_t *)h);
}

void kvspace_bytes_free(uint8_t *p, uint32_t len) {
    (void)len;
    free(p);
}

int kvspace_set(void *h, const char *const *keys, const uint8_t *vals,
                const uint32_t *lens, uint32_t n, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        kvsc_set((kvspace_t *)h, keys[i], vals + off, (int32_t)lens[i]);
        off += lens[i];
    }
    return 0;
}

int kvspace_get_one(void *h, const char *key, uint8_t **out, uint32_t *out_len) {
    int32_t len;
    uint8_t *d = kvsc_get((kvspace_t *)h, key, 1, &len);
    if (!d || len <= 0) { *out = NULL; *out_len = 0; return 0; }
    uint8_t *c = malloc((size_t)len);
    memcpy(c, d, (size_t)len);
    *out = c; *out_len = (uint32_t)len;
    return 0;
}

int kvspace_list(void *h, const char *prefix, int expand_ext, int resolve,
                 uint8_t **out, uint32_t *out_len) {
    char **names; int32_t count;
    if (kvsc_list((kvspace_t *)h, prefix, expand_ext, resolve, &names, &count) != 0) {
        *out = NULL; *out_len = 0; return -1;
    }
    size_t total = 0;
    for (int32_t i = 0; i < count; i++) total += strlen(names[i]) + 1;
    uint8_t *buf = total ? malloc(total) : NULL;
    size_t off = 0;
    for (int32_t i = 0; i < count; i++) {
        size_t l = strlen(names[i]);
        memcpy(buf + off, names[i], l);
        off += l;
        if (i < count - 1) buf[off++] = '\n';
    }
    for (int32_t i = 0; i < count; i++) free(names[i]);
    free(names);
    *out = buf; *out_len = (uint32_t)off;
    return 0;
}

int kvspace_del(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    for (uint32_t i = 0; i < nkeys; i++) kvsc_del((kvspace_t *)h, keys[i]);
    return 0;
}

int kvspace_del_tree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvsc_deltree((kvspace_t *)h, prefix);
}

int kvspace_mkindex(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvsc_mkindex((kvspace_t *)h, path);
}

int kvspace_ext_index(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvsc_extindex((kvspace_t *)h, path, ext_path);
}

int kvspace_del_ext_index(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvsc_delextindex((kvspace_t *)h, path);
}

int kvspace_clear(void *h, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvsc_deltree((kvspace_t *)h, "/");
}

int kvspace_disconn(void *h, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return 0;
}

int kvspace_tlv_encode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                       const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    int32_t n = xvalue_encode(kind, raw, (int32_t)raw_len, dims, ndim, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_tlv_encode_ptr(const char *kind, const uint8_t *raw, uint32_t raw_len,
                           const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    int32_t n = xvalue_encode_ptr(kind, raw, (int32_t)raw_len, dims, ndim, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_decode_head(const uint8_t *data, uint32_t data_len, kvhead_t *out) {
    if (!out) return 1;
    xvalue_head_t h = xvalue_decode_head(data, (int32_t)data_len);
    memset(out, 0, sizeof(*out));
    uint32_t kl = (uint32_t)h.kind_len;
    if (kl > 31) kl = 31;
    memcpy(out->kind, h.kind, kl);
    out->kind[kl] = 0;
    out->is_ptr = (h.ref == 1);
    out->array_len = h.array_len;
    out->body_len = h.raw_len;
    out->body_offset = xvalue_head_len(&h);
    out->ndim = h.ndim;
    for (int i = 0; i < 8 && i < h.ndim; i++) out->dims[i] = h.dims[i];
    return 0;
}

int kvspace_new_ptr(const char *kind, const char *target, int32_t array_len,
                    uint8_t **out, uint32_t *out_len) {
    int32_t n = xvalue_new_ptr(kind, target, array_len, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_new_char(const char *kind, const char *s, uint8_t **out, uint32_t *out_len) {
    int32_t n;
    if (strcmp(kind, XK_CHAR_UTF8) == 0) n = xvalue_new_char_utf8(s, out);
    else if (strcmp(kind, XK_CHAR_ASCII) == 0) n = xvalue_new_char_ascii(s, out);
    else n = xvalue_new_char(s, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_new_char_byte(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    int32_t d = (int32_t)len;
    int32_t n = xvalue_encode(XK_CHAR_UTF8, bytes, (int32_t)len, &d, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_new_bool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    bool b = v != 0;
    int32_t n = xvalue_new_bool(&b, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_new_int64(int64_t v, uint8_t **out, uint32_t *out_len) {
    int32_t n = xvalue_new_int64(&v, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_new_float64(double v, uint8_t **out, uint32_t *out_len) {
    int32_t n = xvalue_new_float64(&v, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspace_get_batch(void *h, const char *prefix, const char *const *names,
                      uint32_t nnames, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return 1;
    *out = NULL;
    *out_len = 0;
    if (!names || nnames == 0) return 0;
    size_t total = (size_t)nnames * 4;
    for (uint32_t i = 0; i < nnames; i++) {
        char key[2048];
        snprintf(key, sizeof key, "%s%s", prefix, names[i]);
        int32_t len = 0;
        uint8_t *d = kvsc_get((kvspace_t *)h, key, 1, &len);
        if (d && len > 0) total += (size_t)len;
    }
    uint8_t *buf = malloc(total);
    if (!buf) return 1;
    size_t off = 0;
    for (uint32_t i = 0; i < nnames; i++) {
        char key[2048];
        snprintf(key, sizeof key, "%s%s", prefix, names[i]);
        int32_t len = 0;
        uint8_t *d = kvsc_get((kvspace_t *)h, key, 1, &len);
        if (!d || len <= 0) len = 0;
        buf[off] = (uint8_t)(len & 0xFF);
        buf[off + 1] = (uint8_t)((len >> 8) & 0xFF);
        buf[off + 2] = (uint8_t)((len >> 16) & 0xFF);
        buf[off + 3] = (uint8_t)((len >> 24) & 0xFF);
        off += 4;
        if (len > 0) {
            memcpy(buf + off, d, (size_t)len);
            off += (size_t)len;
        }
    }
    *out = buf;
    *out_len = (uint32_t)off;
    return 0;
}

int kvspace_watch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                  uint64_t tick_ns, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return 1;
    *out = NULL;
    *out_len = 0;
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int32_t len = 0;
        uint8_t *d = kvsc_get((kvspace_t *)h, key, 1, &len);
        if (d && (uint32_t)len == target_len && memcmp(d, target, target_len) == 0) {
            uint8_t *c = malloc((size_t)len);
            memcpy(c, d, (size_t)len);
            *out = c;
            *out_len = (uint32_t)len;
            return 0;
        }
        clock_gettime(CLOCK_MONOTONIC, &tn);
        uint64_t elapsed = (uint64_t)(tn.tv_sec - t0.tv_sec) * 1000000000ULL +
                           (uint64_t)(tn.tv_nsec - t0.tv_nsec);
        if (elapsed >= tick_ns) return 0;
        usleep(1000);
    }
}
