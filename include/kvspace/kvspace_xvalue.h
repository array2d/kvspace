#ifndef KVSPACE_XVALUE_H
#define KVSPACE_XVALUE_H 
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#if defined(_WIN32)
#define KVSPACE_PUBLIC __declspec(dllexport)
#elif defined(__GNUC__)
#define KVSPACE_PUBLIC __attribute__((visibility("default")))
#else
#define KVSPACE_PUBLIC 
#endif
#if defined(__cplusplus)
#define KVSPACE_STATIC_ASSERT(condition,message) static_assert(condition, message)
#else
#define KVSPACE_STATIC_ASSERT(condition,message) _Static_assert(condition, message)
#endif
#ifdef __cplusplus
extern "C" {
#endif
#define KVSPACE_XVALUE_HEAD_LEN 16u
/* miaobyte, 2026-11-16, 最高, 固定 16 字节 head（结构体直接 memcpy + body + attr） */
typedef struct kvspaceXvalueHead {
    uint64_t bodylen;      /* body 区有效字节数；容量是存储后端的分配属性 */
    uint16_t attrslen;     /* attr 区字节数（不含任何前缀）；上限 UINT16_MAX */
    uint8_t  padding[6];   /* 必须为零；读取方遇到非零按非法处理 */
} kvspaceXvalueHead;
KVSPACE_STATIC_ASSERT(sizeof(kvspaceXvalueHead) == KVSPACE_XVALUE_HEAD_LEN,
               "XValue head must be exactly 16 bytes");
KVSPACE_STATIC_ASSERT(offsetof(kvspaceXvalueHead, bodylen) == 0U, "bodylen@0");
KVSPACE_STATIC_ASSERT(offsetof(kvspaceXvalueHead, attrslen) == 8U, "attrslen@8");
KVSPACE_STATIC_ASSERT(offsetof(kvspaceXvalueHead, padding) == 10U, "padding@10");
static inline uint64_t kvspaceXvalueHeadLen(const kvspaceXvalueHead *h) {
    return h->bodylen;
}
static inline void kvspaceXvalueHeadSetLen(kvspaceXvalueHead *h, uint64_t v) {
    h->bodylen = v;
}
static inline uint16_t kvspaceXvalueHeadAttrsLen(const kvspaceXvalueHead *h) {
    return h->attrslen;
}
static inline void kvspaceXvalueHeadSetAttrsLen(kvspaceXvalueHead *h, uint16_t v) {
    h->attrslen = v;
}
static inline int kvspaceXvalueHeadPaddingIsZero(const kvspaceXvalueHead *h) {
    return h->padding[0] == 0u && h->padding[1] == 0u &&
           h->padding[2] == 0u && h->padding[3] == 0u &&
           h->padding[4] == 0u && h->padding[5] == 0u;
}
static inline uint64_t kvspaceXvalueHeadTotalLen(const kvspaceXvalueHead *h) {
    if (!kvspaceXvalueHeadPaddingIsZero(h))
        return 0;
    const uint64_t body = kvspaceXvalueHeadLen(h);
    const uint64_t attrs = kvspaceXvalueHeadAttrsLen(h);
    if (body > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN ||
        attrs > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN - body)
        return 0;
    return (uint64_t)KVSPACE_XVALUE_HEAD_LEN + body + attrs;
}
static inline const uint8_t *kvspaceXvalueHeadAttrs(const uint8_t *wire) {
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    return wire + KVSPACE_XVALUE_HEAD_LEN + (uint64_t)h->bodylen;
}
#define KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN 2u
/* miaobyte, 2026-11-16, 最高, tlv编码 */
int kvspaceXvalueGetAttr(const uint8_t *wire, uint64_t wire_len, uint8_t tag,
                          const uint8_t **out_data, uint16_t *out_len);
int kvspaceXvalueSetAttr(uint8_t *wire, uint64_t wire_capacity, uint8_t tag,
                          const uint8_t *data, uint16_t len,
                          uint64_t *out_new_length);
int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire, uint64_t wire_len,
                                    uint64_t *out);
#ifdef __cplusplus
}
#endif
#endif
