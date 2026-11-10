/* Wire: [bodylen:u64 LE@0][attrslen:u16 LE@8][padding:6@10-15]
   [body @16..16+bodylen][attr @16+bodylen..16+bodylen+attrslen].
   Head is fixed 16 bytes; body offset is always 16.
   head 只描述「字节怎么读」：bodylen 恒为 body 区有效字节数；容量是存储后端的
   物理分配属性，不进入 wire，也不改变 bodylen 的含义。
   The attr region carries NO length prefix — attrslen is in the head, so a second
   prefix would be a second source of truth. Total = 16 + bodylen + attrslen,
   computed from the head alone.
   Type rides in the same single-key value — no attr key. attr record =
   [tag:u8][len:u16 LE][data]; tag 1 = langtype, tags 3+ reserved. tag 2
   (旧 reallen) 已删除，出现即拒绝。
   Wire is little-endian. Current platforms are LE, so a straight memcpy of the
   struct equals the wire; a big-endian target must convert at the boundary. */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>


/* C++ 不认 `_Static_assert`（C11 关键字）；同一份断言给两种语言用。 */
#if defined(__cplusplus)
#define KVSPACE_STATIC_ASSERT(condition, message) static_assert(condition, message)
#else
#define KVSPACE_STATIC_ASSERT(condition, message) _Static_assert(condition, message)
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_XVALUE_HEAD_LEN 16u

/* Fixed 16-byte layout (direct memcpy struct + body + attrs).
   wire: bodylen:u64@0, attrslen:u16@8, padding@10-15. */
typedef struct kvspaceXvalueHead {
    uint64_t bodylen;      /* body 区有效字节数；容量是存储后端分配属性 */
    uint16_t attrslen;     /* attr 区字节数（不含任何前缀）；上限 UINT16_MAX */
    uint8_t  padding[6];   /* 必须为零；读取方遇到非零按非法处理 */
} kvspaceXvalueHead;

KVSPACE_STATIC_ASSERT(sizeof(kvspaceXvalueHead) == KVSPACE_XVALUE_HEAD_LEN,
               "XValue head must be exactly 16 bytes");
KVSPACE_STATIC_ASSERT(offsetof(kvspaceXvalueHead, bodylen) == 0U, "bodylen@0");
KVSPACE_STATIC_ASSERT(offsetof(kvspaceXvalueHead, attrslen) == 8U, "attrslen@8");
KVSPACE_STATIC_ASSERT(offsetof(kvspaceXvalueHead, padding) == 10U, "padding@10");

/* 字段的唯一定义点：读/写一律走这些 inline 方法，禁止各处直接取字段或手写偏移。 */
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
/* padding 是 ABI 的一部分：非零即非法，读取路径必须先过这一关。 */
static inline int kvspaceXvalueHeadPaddingIsZero(const kvspaceXvalueHead *h) {
    return h->padding[0] == 0u && h->padding[1] == 0u &&
           h->padding[2] == 0u && h->padding[3] == 0u &&
           h->padding[4] == 0u && h->padding[5] == 0u;
}
/* 总长 = 16 + bodylen + attrslen；非零 padding、溢出都返回 0
   （0 不是合法总长）。 */
static inline uint64_t kvspaceXvalueHeadTotalLen(const kvspaceXvalueHead *h) {
    if (!kvspaceXvalueHeadPaddingIsZero(h))
        return 0;
    uint64_t body = kvspaceXvalueHeadLen(h);
    uint64_t attrs = kvspaceXvalueHeadAttrsLen(h);
    if (body > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN ||
        attrs > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN - body)
        return 0;
    return (uint64_t)KVSPACE_XVALUE_HEAD_LEN + body + attrs;
}
/* attr 区起点：总由 head 推出，不传 body_len。 */
static inline const uint8_t *kvspaceXvalueHeadAttrs(const uint8_t *wire) {
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    return wire + KVSPACE_XVALUE_HEAD_LEN + (uint64_t)h->bodylen;
}

/* attr 区常量：record tag 1 = langtype；tag 2 是已删除的 reallen，读取时拒绝。 */
#define KVSPACE_XVALUE_ATTR_TAG_LANGTYPE 1u

/* attr 扫描的唯一入口：校验 padding 与总长，取出 tag 1。
   输出参数都可为 NULL。返回 0 成功，-1 失败。
   tag 2（旧 reallen）已删除，出现即拒绝，避免把旧容量 wire 误读成有效长度。 */
int kvspaceXvalueHeadScan(const uint8_t *wire, uint64_t wire_len,
                           const uint8_t **out_langtype,
                           uint16_t *out_langtype_len);

int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire, uint64_t wire_len,
                                    uint64_t *out);

/* Scan the attr region for tag=1 (langtype) data; returns 0 if missing or
   malformed. Unbounded: caller must ensure the wire holds 16+bodylen+attrslen
   bytes; use kvspaceXvalueHeadLangtypeBounded when bounds are not yet verified.
   The attr region has no length prefix — its size comes from the head. */
static inline uint16_t kvspaceXvalueHeadLangtype(const uint8_t *wire,
                                                 const uint8_t **out) {
    if (out)
        *out = NULL;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0)
        return 0;
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    if (kvspaceXvalueHeadScan(wire, total, &langtype, &langtype_len) != 0)
        return 0;
    if (out)
        *out = langtype;
    return langtype_len;
}

/* Bounded variant: verifies the head fits, padding is zero and
   16+bodylen+attrslen <= wire_len before scanning records.
   Returns 0 and sets *out=NULL on missing/malformed/truncated values. */
static inline uint16_t kvspaceXvalueHeadLangtypeBounded(const uint8_t *wire,
                                                        uint64_t wire_len,
                                                        const uint8_t **out) {
    if (out)
        *out = NULL;
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    if (kvspaceXvalueHeadScan(wire, wire_len, &langtype, &langtype_len) != 0)
        return 0;
    if (out)
        *out = langtype;
    return langtype_len;
}

/* Constructors (fixed 16-byte header + body + attr region in a single value).
   Empty/NULL langtype omits the langtype record (attrslen = 0).
   Returns 0 on success, -1 and clears *out on failure (when out is non-NULL).

   `bodylen` 恒为有效字节数；容量是存储后端的物理分配属性，不进入 wire。
   构造器不再有 capacity 参数，也不再写 attr tag 2。

   NewPtr/NewExt 是唯一知道「这个值是引用」的构造点：调用方传 pointed-to 的
   基 kindexpr（不带 * / @），构造器补前缀（空 langtype → 裸 "*"/"@"）。
   带 * / @ 的输入直接拒绝——双重前缀说明两处对格式理解不一致。

   **`@` 仅保留线格式构造**：NewExt 能产出带 `@` 前缀的合法 wire，但运行时
   语义（读、写、比较、解引用）本期不实现、不消费——线格式要先立住，语义后补。 */
int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewScalar(const uint8_t *raw, uint32_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              const char *langtype,
                              uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len);
int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             const char *langtype,
                             uint8_t **out, uint64_t *out_len);
/* 预留 = body 区 body_len 字节零填；容量由存储后端的 reserve 参数承担。 */
int kvspaceXvalueHeadReserve(uint64_t body_len, const char *langtype,
                             uint8_t **out, uint64_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
