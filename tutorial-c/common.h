// common.h — tutorial 公共助手：统一 kvspace* ABI 的编码/解码与单键读写便捷封装。
// 所有 tutorial 均链接 libkvspace（dispatch 前端），运行期经 shm:// 选择 kvspace-c 后端。
#ifndef KVSPACE_TUTORIAL_COMMON_H
#define KVSPACE_TUTORIAL_COMMON_H

#include "kvspace/kvspace.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── 编码：返回 malloc 的 TLV 字节，长度写入 *len，调用方 free ──

static inline uint8_t *enc_int64(int64_t v, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceNewInt64(v, &out, &n);
    *len = n; return out;
}

static inline uint8_t *enc_str(const char *s, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceNewCharByte((const uint8_t *)s, (uint32_t)strlen(s), &out, &n);
    *len = n; return out;
}

static inline uint8_t *enc_bytes(const uint8_t *raw, uint32_t rl, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    int32_t dims[1] = { (int32_t)rl };
    kvspaceTlvEncode("uint8", raw, rl, dims, 1, &out, &n);
    *len = n; return out;
}

// ── 解码：填 base kind（去 */@ 前缀与 [dims] 段），返回 body 指针/长度 ──

static inline const char *dec(const uint8_t *data, uint32_t len, char *kind, size_t cap,
                              const uint8_t **body, uint32_t *body_len) {
    kvspaceHead_t h;
    if (kvspaceDecodeHead(data, len, &h) != 0) return NULL;
    const char *k = (const char *)h.kindexpr;
    if (k[0] == '*' || k[0] == '@') k++;
    const char *base = k;
    if (k[0] == '[') { const char *e = strchr(k, ']'); if (e) base = e + 1; }
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(kind, base, n); kind[n] = 0;
    *body = data + h.body_offset;
    *body_len = h.body_len > 0 ? (uint32_t)h.body_len : 0;
    return kind;
}

// ── 单键读写封装（ABI 为批量，单键场景更顺） ──

static inline int kv_set(void *h, const char *key, const uint8_t *val, uint32_t len) {
    const char *keys[1] = { key };
    const uint32_t lens[1] = { len };
    return kvspaceSet(h, keys, val, lens, 1, NULL, 0);
}

static inline uint8_t *kv_get(void *h, const char *key, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceGet(h, key, &out, &n);
    *len = n; return out;
}

static inline void kv_del(void *h, const char *key) {
    const char *keys[1] = { key };
    kvspaceDel(h, keys, 1, NULL, 0);
}

// ── 连接：shm:// DSN → dispatch 前端 → kvspace-c 后端 ──

static inline void *kv_open_shm(const char *path) {
    size_t n = strlen(path) + 7;
    char *dsn = malloc(n);
    snprintf(dsn, n, "shm://%s", path);
    void *h = kvspaceConnect(dsn);
    free(dsn);
    return h;
}

#endif
