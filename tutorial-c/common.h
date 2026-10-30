// common.h — tutorial 公共助手：统一 kvspace* ABI 的 XValue 构造/读取与单键读写便捷封装。
// 所有 tutorial 均链接 libkvspace（dispatch 前端），运行期经 shm:// 选择 kvspace-c 后端。
// #566：head 固定 8 字节（body 偏移恒 8），类型/形状走 x‥langtype attr key。
#ifndef KVSPACE_TUTORIAL_COMMON_H
#define KVSPACE_TUTORIAL_COMMON_H

#include "kvspace/kvspace.h"
#include <kvspace/xvalue_head.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ── 构造：返回 malloc 的 XValue 字节（固定头 + body），长度写入 *len，调用方 free ──

static inline uint8_t *new_int64(int64_t v, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceNewInt64(v, &out, &n);
    *len = n; return out;
}

static inline uint8_t *new_str(const char *s, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceNewChar((const uint8_t *)s, (uint32_t)strlen(s), &out, &n);
    *len = n; return out;
}

static inline uint8_t *new_bytes(const uint8_t *raw, uint32_t rl, uint32_t *len) {
    uint8_t *out = NULL; uint64_t n = 0;
    uint64_t dims[1] = { rl };
    kvspaceXvalueHeadNewTensor(dims, 1, raw, rl, &out, &n);
    *len = n > UINT32_MAX ? 0 : (uint32_t)n;
    return out;
}

// ── 读取：kvspaceGetHead 读 x‥langtype attr key 取 base kind（去 */@ 前缀与 [dims] 段），
//    返回 body 指针/长度（body 偏移恒 8）──

static inline const char *read_kind(void *h, const char *key, const uint8_t *data, uint32_t len,
                              char *kind, size_t cap,
                              const uint8_t **body, uint32_t *body_len) {
    if (!h || !key || !data || len < KVSPACE_XVALUE_HEAD_LEN) return NULL;
    kvspaceHead_t hd;
    if (kvspaceGetHead(h, key, &hd) != 0) return NULL;
    const char *k = (const char *)hd.langtype;
    if (k[0] == '*' || k[0] == '@') k++;
    const char *base = k;
    if (k[0] == '[') { const char *e = strchr(k, ']'); if (e) base = e + 1; }
    size_t n = strlen(base);
    if (n >= cap) n = cap - 1;
    memcpy(kind, base, n); kind[n] = 0;
    *body = data + KVSPACE_XVALUE_HEAD_LEN;
    *body_len = len - KVSPACE_XVALUE_HEAD_LEN;
    return kind;
}

// ── 单键读写封装（写：New* 构造 XValue + 显式 langtype → WriteNewPlace 顺写 attr key+body；
//    就地/新位置二选一原语）──

static inline int kv_set(void *h, const char *key, const char *langtype,
                         const uint8_t *val, uint32_t len) {
    if (!val || len < KVSPACE_XVALUE_HEAD_LEN) return -1;
    kvspaceXvalueHead hd;
    memcpy(&hd, val, sizeof hd);
    uint64_t bl = kvspaceXvalueHeadLen(&hd);
    uint8_t *dst = NULL; char err[256];
    if (kvspaceWriteInPlace(h, key, 1, (uint32_t)bl, &dst, err, sizeof err) != 0 &&
        kvspaceWriteNewPlace(h, key, hd.ptr, hd.storetype, langtype,
                             (uint32_t)bl, bl, &dst, err, sizeof err) != 0)
        return -1;
    if (bl && dst) memcpy(dst, val + KVSPACE_XVALUE_HEAD_LEN, (size_t)bl);
    return 0;
}

// 借用读 + 拷出自持：调用方以 libc free 释放（借用本身不得 free）。
static inline uint8_t *kv_get(void *h, const char *key, uint32_t *len) {
    uint8_t *out = NULL; uint32_t n = 0;
    kvspaceGet(h, key, 0, &out, &n);
    if (!out || n == 0) { *len = 0; return NULL; }
    uint8_t *copy = malloc(n);
    memcpy(copy, out, n);
    *len = n; return copy;
}

// 前缀枚举：ListLen 定计数 + 逐 idx ListAt 取名（自备缓冲），拼成 \n 分隔的 malloc 缓冲（调用方 free）。
static inline int kv_list(void *h, const char *prefix, uint8_t **out, uint32_t *out_len) {
    *out = NULL; *out_len = 0;
    int32_t count = 0;
    if (kvspaceListLen(h, prefix, 0, 1, &count) != 0 || count <= 0) return 0;
    size_t cap = 256, used = 0;
    uint8_t *buf = malloc(cap);
    for (int32_t i = 0; i < count; i++) {
        uint8_t nm[1024]; uint32_t nl = 0;
        if (kvspaceListAt(h, prefix, 0, 1, i, nm, sizeof(nm), &nl) != 0 || nl == 0) continue;
        if (used + nl + 1 > cap) { while (used + nl + 1 > cap) cap *= 2; buf = realloc(buf, cap); }
        if (used > 0) buf[used++] = '\n';
        memcpy(buf + used, nm, nl); used += nl;
    }
    *out = buf; *out_len = (uint32_t)used;
    return 0;
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
