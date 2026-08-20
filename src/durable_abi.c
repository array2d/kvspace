/* durable_abi.c — 对齐 kvspace-durable 的 C ABI（kvspaceConnect 等 22 符号），
 * 让 kvlang 的 Rust layout 零改动对接 kvspace-c 的 SHM。内部复用 kvspaceShm* 与 kvspaceXvalue*。 */

#define _POSIX_C_SOURCE 199309L
#define _DEFAULT_SOURCE

#include "kvspace/kvspace.h"
#include "kvspace/xvalue.h"
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SHM_DEFAULT_SIZE (8ULL * 64 * 64 * 64 * 64)

/* 对齐 kvspace-durable 的 kvspaceHead_t（repr(C)）。kind+ndim+dims 即完整 kindexp。 */
typedef struct {
    uint8_t kind[32];
    uint8_t is_ptr;
    int32_t array_len;
    int32_t body_len;
    int32_t body_offset;
    int32_t ndim;
    int32_t dims[8];
} kvspaceHead_t;

static int parse_shm_path(const char *dsn, char *out, size_t osz) {
    const char *sep = strstr(dsn, "://");
    if (!sep || strncmp(dsn, "shm", (size_t)(sep - dsn)) != 0) return -1;
    snprintf(out, osz, "%s", sep + 3);
    return 0;
}

void *kvspaceConnect(const char *dsn) {
    char path[1024];
    if (parse_shm_path(dsn, path, sizeof path) != 0) return NULL;
    return (void *)kvspaceShmOpen(path, SHM_DEFAULT_SIZE);
}

void kvspaceFree(void *h) {
    if (h) kvspaceShmClose((kvspace_t *)h);
}

void kvspaceBytesFree(uint8_t *p, uint32_t len) {
    (void)len;
    free(p);
}

int kvspaceSet(void *h, const char *const *keys, const uint8_t *vals,
                const uint32_t *lens, uint32_t n, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    uint32_t off = 0;
    for (uint32_t i = 0; i < n; i++) {
        kvspaceShmSet((kvspace_t *)h, keys[i], vals + off, (int32_t)lens[i]);
        off += lens[i];
    }
    return 0;
}

int kvspaceGet(void *h, const char *key, uint8_t **out, uint32_t *out_len) {
    int32_t len;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
    if (!d || len <= 0) { *out = NULL; *out_len = 0; return 0; }
    uint8_t *c = malloc((size_t)len);
    memcpy(c, d, (size_t)len);
    *out = c; *out_len = (uint32_t)len;
    return 0;
}

int kvspaceList(void *h, const char *prefix, int expand_ext, int resolve,
                 uint8_t **out, uint32_t *out_len) {
    char **names; int32_t count;
    if (kvspaceShmList((kvspace_t *)h, prefix, expand_ext, resolve, &names, &count) != 0) {
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

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    for (uint32_t i = 0; i < nkeys; i++) kvspaceShmDel((kvspace_t *)h, keys[i]);
    return 0;
}

int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, prefix);
}

int kvspaceMkindex(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmMkindex((kvspace_t *)h, path);
}

int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmExtindex((kvspace_t *)h, path, ext_path);
}

int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmDelextindex((kvspace_t *)h, path);
}

int kvspaceClear(void *h, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return kvspaceShmDeltree((kvspace_t *)h, "/");
}

int kvspaceDisconnect(void *h, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    return 0;
}

int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                       const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueEncode(kind, raw, (int32_t)raw_len, dims, ndim, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceTlvEncodePtr(const char *kind, const uint8_t *raw, uint32_t raw_len,
                           const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueEncodePtr(kind, raw, (int32_t)raw_len, dims, ndim, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out) {
    if (!out) return 1;
    xvalue_head_t h = kvspaceXvalueDecodeHead(data, (int32_t)data_len);
    memset(out, 0, sizeof(*out));
    uint32_t kl = (uint32_t)h.kind_len;
    if (kl > 31) kl = 31;
    memcpy(out->kind, h.kind, kl);
    out->kind[kl] = 0;
    out->is_ptr = (h.ref == 1);
    out->array_len = h.array_len;
    out->body_len = h.raw_len;
    out->body_offset = kvspaceXvalueHeadLen(&h);
    out->ndim = h.ndim;
    for (int i = 0; i < 8 && i < h.ndim; i++) out->dims[i] = h.dims[i];
    return 0;
}

int kvspaceNewPtr(const char *kind, const char *target, int32_t array_len,
                    uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewPtr(kind, target, array_len, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewChar(const char *kind, const char *s, uint8_t **out, uint32_t *out_len) {
    int32_t n;
    if (strcmp(kind, KVSPACE_KIND_CHAR_UTF8) == 0) n = kvspaceXvalueNewCharUtf8(s, out);
    else if (strcmp(kind, KVSPACE_KIND_CHAR_ASCII) == 0) n = kvspaceXvalueNewCharAscii(s, out);
    else n = kvspaceXvalueNewChar(s, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewCharByte(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    int32_t d = (int32_t)len;
    int32_t n = kvspaceXvalueEncode(KVSPACE_KIND_CHAR_UTF8, bytes, (int32_t)len, &d, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    bool b = v != 0;
    int32_t n = kvspaceXvalueNewBool(&b, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewInt64(&v, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    int32_t n = kvspaceXvalueNewFloat64(&v, 1, out);
    if (n < 0) return 1;
    *out_len = (uint32_t)n;
    return 0;
}

int kvspaceGetBatch(void *h, const char *prefix, const char *const *names,
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
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
        if (d && len > 0) total += (size_t)len;
    }
    uint8_t *buf = malloc(total);
    if (!buf) return 1;
    size_t off = 0;
    for (uint32_t i = 0; i < nnames; i++) {
        char key[2048];
        snprintf(key, sizeof key, "%s%s", prefix, names[i]);
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
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

int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                  uint64_t tick_ns, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return 1;
    *out = NULL;
    *out_len = 0;
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        int32_t len = 0;
        uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
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

/* Durable Notify/Take queue. Outside the user-visible tree (old Watch is WatchValue). */
#define NQ_PFX "/\xE2\x80\xA5notify"

static pthread_mutex_t nq_mu = PTHREAD_MUTEX_INITIALIZER;

static void nq_key(const char *key, char *out, size_t cap) {
    snprintf(out, cap, "%s%s", NQ_PFX, key ? key : "");
}

static uint32_t nq_rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void nq_wr32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static int nq_load(kvspace_t *kv, const char *qk, uint8_t **body, uint32_t *blen) {
    int32_t len = 0;
    uint8_t *d = kvspaceShmGet(kv, qk, 1, &len);
    *body = NULL; *blen = 0;
    if (!d || len <= 0) return 0;
    xvalue_head_t h = kvspaceXvalueDecodeHead(d, len);
    if (h.raw_len <= 0 || !h.raw) return 0;
    uint8_t *c = malloc((size_t)h.raw_len);
    if (!c) abort();
    memcpy(c, h.raw, (size_t)h.raw_len);
    *body = c; *blen = (uint32_t)h.raw_len;
    return 0;
}

static int nq_save(kvspace_t *kv, const char *qk, const uint8_t *body, uint32_t blen) {
    if (blen == 0) return kvspaceShmDel(kv, qk);
    int32_t dims[1] = { (int32_t)blen };
    uint8_t *tlv = NULL;
    int32_t n = kvspaceXvalueEncode(KVSPACE_KIND_UINT8, body, (int32_t)blen, dims, 1, &tlv);
    if (n < 0 || !tlv) abort();
    int rc = kvspaceShmSet(kv, qk, tlv, n);
    free(tlv);
    return rc;
}

int kvspaceNotify(void *h, const char *key, const uint8_t *val, uint32_t len, char *err, uint32_t err_cap) {
    (void)err; (void)err_cap;
    if (!h || !key || !val || len == 0) return 1;
    char qk[2048];
    nq_key(key, qk, sizeof qk);
    pthread_mutex_lock(&nq_mu);
    uint8_t *body = NULL; uint32_t blen = 0;
    nq_load((kvspace_t *)h, qk, &body, &blen);
    uint8_t *nb = malloc((size_t)blen + 4 + len);
    if (!nb) abort();
    if (blen) memcpy(nb, body, blen);
    nq_wr32(nb + blen, len);
    memcpy(nb + blen + 4, val, len);
    int rc = nq_save((kvspace_t *)h, qk, nb, blen + 4 + len);
    free(nb); free(body);
    pthread_mutex_unlock(&nq_mu);
    return rc == 0 ? 0 : 1;
}

int kvspaceTake(void *h, const char *key, uint64_t timeout_ns, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len) return 1;
    *out = NULL; *out_len = 0;
    if (!h || !key) return 1;
    char qk[2048];
    nq_key(key, qk, sizeof qk);
    struct timespec t0, tn;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (;;) {
        pthread_mutex_lock(&nq_mu);
        uint8_t *body = NULL; uint32_t blen = 0;
        nq_load((kvspace_t *)h, qk, &body, &blen);
        if (blen >= 4) {
            uint32_t fl = nq_rd32(body);
            if (4 + fl <= blen) {
                uint8_t *item = malloc(fl ? fl : 1);
                if (!item) abort();
                if (fl) memcpy(item, body + 4, fl);
                uint32_t rest = blen - 4 - fl;
                nq_save((kvspace_t *)h, qk, body + 4 + fl, rest);
                free(body);
                pthread_mutex_unlock(&nq_mu);
                *out = item; *out_len = fl;
                return 0;
            }
        }
        free(body);
        pthread_mutex_unlock(&nq_mu);
        clock_gettime(CLOCK_MONOTONIC, &tn);
        uint64_t elapsed = (uint64_t)(tn.tv_sec - t0.tv_sec) * 1000000000ULL +
                           (uint64_t)(tn.tv_nsec - t0.tv_nsec);
        if (elapsed >= timeout_ns) return 0;
        usleep(1000);
    }
}

static pthread_mutex_t incr_mu = PTHREAD_MUTEX_INITIALIZER;

static void incr_err(char *err, uint32_t err_cap, const char *msg) {
    if (!err || err_cap == 0) return;
    snprintf(err, err_cap, "%s", msg);
}

int kvspaceIncr(void *h, const char *key, int64_t *out, char *err, uint32_t err_cap) {
    if (!h || !key || !out) { incr_err(err, err_cap, "Incr: bad args"); return 1; }
    *out = 0;
    pthread_mutex_lock(&incr_mu);
    int32_t len = 0;
    uint8_t *d = kvspaceShmGet((kvspace_t *)h, key, 1, &len);
    int64_t n = 0;
    if (d && len > 0) {
        xvalue_head_t hd = kvspaceXvalueDecodeHead(d, len);
        if (hd.kind_len < 5 || memcmp(hd.kind, "char/", 5) != 0 || !hd.raw || hd.raw_len <= 0) {
            pthread_mutex_unlock(&incr_mu);
            incr_err(err, err_cap, "Incr: counter is not a Char");
            return 1;
        }
        char *s = malloc((size_t)hd.raw_len + 1);
        if (!s) abort();
        memcpy(s, hd.raw, (size_t)hd.raw_len);
        s[hd.raw_len] = 0;
        char *end = NULL;
        n = strtoll(s, &end, 10);
        int bad = end == s || (end && *end);
        free(s);
        if (bad) {
            pthread_mutex_unlock(&incr_mu);
            incr_err(err, err_cap, "Incr: unparsable counter");
            return 1;
        }
    }
    if (n == INT64_MAX) {
        pthread_mutex_unlock(&incr_mu);
        incr_err(err, err_cap, "Incr: overflow");
        return 1;
    }
    n++;
    char buf[32];
    snprintf(buf, sizeof buf, "%lld", (long long)n);
    uint8_t *tlv = NULL;
    int32_t tn = kvspaceXvalueNewCharUtf8(buf, &tlv);
    if (tn < 0 || !tlv) abort();
    int rc = kvspaceShmSet((kvspace_t *)h, key, tlv, tn);
    free(tlv);
    pthread_mutex_unlock(&incr_mu);
    if (rc != 0) { incr_err(err, err_cap, "Incr: set failed"); return 1; }
    *out = n;
    return 0;
}
