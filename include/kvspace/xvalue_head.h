/* Wire: [bodylen:u64 LE@0][attrslen:u32 LE@8][layout:u8@12][padding:3@13-15]
   [body @16..16+bodylen][attr @16+bodylen..16+bodylen+attrslen].
   Head is fixed 16 bytes; body offset is always 16.
   head 只描述「字节怎么读」：bodylen 是 body 区字节数，layout 说它是不是容量。
   「这个值是什么」全在 langtype（attr tag 1，含 [dims] 与 * / @ 前缀），head 不承载。
   The attr region carries NO length prefix — attrslen is in the head, so a second
   prefix would be a second source of truth. Total = 16 + bodylen + attrslen,
   computed from the head alone.
   Type rides in the same single-key value — no attr key. attr record =
   [tag:u8][len:u16 LE][data]; tag 1 = langtype, tag 2 = reallen, tags 3+ reserved.
   Wire is little-endian. Current platforms are LE, so a straight memcpy of the
   struct equals the wire; a big-endian target must convert at the boundary. */
#ifndef XVALUE_HEAD_H
#define XVALUE_HEAD_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>


#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_XVALUE_HEAD_LEN 16u

/* layout：bodylen 的语义。没有 storetype/ptr 字段——存储形态与引用性都从
   langtype 推出；唯一推不出的是「body 区是容量而非有效长度」，故只保留这一标志。 */
#define KVSPACE_XVALUE_LAYOUT_LENGTH   0u /* bodylen = 有效长度 */
#define KVSPACE_XVALUE_LAYOUT_CAPACITY 1u /* bodylen = 容量；有效长度在 attr tag 2 */

/* Fixed 16-byte layout (direct memcpy struct + body + attrs).
   wire: bodylen:u64@0, attrslen:u32@8, layout@12, padding@13-15. */
typedef struct kvspaceXvalueHead {
    uint64_t bodylen;      /* body 区字节数：有效长度，或容量（见 layout） */
    uint32_t attrslen;     /* attr 区字节数（不含任何前缀） */
    uint8_t  layout;       /* KVSPACE_XVALUE_LAYOUT_*；其它值非法 */
    uint8_t  padding[3];   /* 必须为零；读取方遇到非零按非法处理 */
} kvspaceXvalueHead;

_Static_assert(sizeof(kvspaceXvalueHead) == KVSPACE_XVALUE_HEAD_LEN,
               "XValue head must be exactly 16 bytes");
_Static_assert(offsetof(kvspaceXvalueHead, bodylen) == 0U, "bodylen@0");
_Static_assert(offsetof(kvspaceXvalueHead, attrslen) == 8U, "attrslen@8");
_Static_assert(offsetof(kvspaceXvalueHead, layout) == 12U, "layout@12");
_Static_assert(offsetof(kvspaceXvalueHead, padding) == 13U, "padding@13");

/* 字段的唯一定义点：读/写一律走这些 inline 方法，禁止各处直接取字段或手写偏移。 */
static inline uint64_t kvspaceXvalueHeadLen(const kvspaceXvalueHead *h) {
    return h->bodylen;
}
static inline void kvspaceXvalueHeadSetLen(kvspaceXvalueHead *h, uint64_t v) {
    h->bodylen = v;
}
static inline uint32_t kvspaceXvalueHeadAttrsLen(const kvspaceXvalueHead *h) {
    return h->attrslen;
}
static inline void kvspaceXvalueHeadSetAttrsLen(kvspaceXvalueHead *h, uint32_t v) {
    h->attrslen = v;
}
static inline uint8_t kvspaceXvalueHeadLayout(const kvspaceXvalueHead *h) {
    return h->layout;
}
static inline void kvspaceXvalueHeadSetLayout(kvspaceXvalueHead *h, uint8_t v) {
    h->layout = v;
}
static inline int kvspaceXvalueHeadLayoutIsValid(const kvspaceXvalueHead *h) {
    return h->layout == KVSPACE_XVALUE_LAYOUT_LENGTH ||
           h->layout == KVSPACE_XVALUE_LAYOUT_CAPACITY;
}
/* padding 是 ABI 的一部分：非零即非法，读取路径必须先过这一关。 */
static inline int kvspaceXvalueHeadPaddingIsZero(const kvspaceXvalueHead *h) {
    return h->padding[0] == 0u && h->padding[1] == 0u && h->padding[2] == 0u;
}
/* 总长 = 16 + bodylen + attrslen；非法 layout、非零 padding、溢出都返回 0
   （0 不是合法总长）。 */
static inline uint64_t kvspaceXvalueHeadTotalLen(const kvspaceXvalueHead *h) {
    if (!kvspaceXvalueHeadPaddingIsZero(h) || !kvspaceXvalueHeadLayoutIsValid(h))
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

/* attr 区常量：record tag 1 = langtype；tag 2 = reallen；其余预留。 */
#define KVSPACE_XVALUE_ATTR_TAG_LANGTYPE 1u
/* tag 2 的归属是**排他**的：layout=1 必须恰有一条、layout=0 必须不带。
   未知 tag 一般忽略以保持前向兼容；tag 2 例外——它已分配给 reallen，而 reallen
   只在 layout=1 下有定义。「有定义但在此 layout 下无意义」说明写读双方对格式
   理解不一致，必须拒绝；「我不认识这个 tag」才该忽略。
   data 恒 8 字节 u64 LE。 */
#define KVSPACE_XVALUE_ATTR_TAG_REALLEN 2u

/* attr 扫描的唯一入口：校验 padding、layout、总长与 tag 2 归属，取出 tag 1
   与 tag 2。输出参数都可为 NULL。返回 0 成功，-1 失败。 */
static inline int kvspaceXvalueHeadScan(const uint8_t *wire, uint64_t wire_len,
                                        const uint8_t **out_langtype,
                                        uint16_t *out_langtype_len,
                                        uint64_t *out_reallen) {
    if (out_langtype)
        *out_langtype = NULL;
    if (out_langtype_len)
        *out_langtype_len = 0;
    if (out_reallen)
        *out_reallen = 0;
    if (!wire || wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_len)
        return -1;
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + h->bodylen;
    const uint8_t *end = p + h->attrslen;
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    uint64_t reallen = 0;
    int seen_reallen = 0;
    while (p + 3 <= end) {
        uint8_t tag = p[0];
        uint16_t rlen = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if (p + 3 + rlen > end)
            return -1;
        if (tag == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE) {
            if (!langtype) { /* 重复 langtype 取第一条：单 key 只有一个写方。 */
                langtype = p + 3;
                langtype_len = rlen;
            }
        } else if (tag == KVSPACE_XVALUE_ATTR_TAG_REALLEN) {
            if (h->layout != KVSPACE_XVALUE_LAYOUT_CAPACITY || seen_reallen ||
                rlen != 8)
                return -1;
            for (int i = 0; i < 8; i++)
                reallen |= (uint64_t)p[3 + i] << (8 * i);
            seen_reallen = 1;
        }
        p += 3 + rlen;
    }
    if (h->layout == KVSPACE_XVALUE_LAYOUT_CAPACITY &&
        (!seen_reallen || reallen > h->bodylen))
        return -1;
    if (out_langtype)
        *out_langtype = langtype;
    if (out_langtype_len)
        *out_langtype_len = langtype_len;
    if (out_reallen)
        *out_reallen = reallen;
    return 0;
}

/* 有效长度：layout=0 时是 bodylen；layout=1 时是 attr tag 2 的 reallen。
   返回 0 成功，-1 失败（*out 置 0；0 本身是合法长度，故错误不走长度值）。 */
static inline int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire,
                                                uint64_t wire_len,
                                                uint64_t *out) {
    if (out)
        *out = 0;
    if (!out)
        return -1;
    uint64_t reallen = 0;
    if (kvspaceXvalueHeadScan(wire, wire_len, NULL, NULL, &reallen) != 0)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    *out = h->layout == KVSPACE_XVALUE_LAYOUT_CAPACITY ? reallen : h->bodylen;
    return 0;
}

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
    if (kvspaceXvalueHeadScan(wire, total, &langtype, &langtype_len, NULL) != 0)
        return 0;
    if (out)
        *out = langtype;
    return langtype_len;
}

/* Bounded variant: verifies the head fits, padding is zero, layout is valid and
   16+bodylen+attrslen <= wire_len before scanning records.
   Returns 0 and sets *out=NULL on missing/malformed/truncated values. */
static inline uint16_t kvspaceXvalueHeadLangtypeBounded(const uint8_t *wire,
                                                        uint64_t wire_len,
                                                        const uint8_t **out) {
    if (out)
        *out = NULL;
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    if (kvspaceXvalueHeadScan(wire, wire_len, &langtype, &langtype_len, NULL) != 0)
        return 0;
    if (out)
        *out = langtype;
    return langtype_len;
}

/* Constructors (fixed 16-byte header + body + attr region in a single value).
   Empty/NULL langtype omits the langtype record (attrslen = 0).
   Returns 0 on success, -1 and clears *out on failure (when out is non-NULL).

   layout 判据：只有「容量 > 有效长度」才是 LAYOUT_CAPACITY（此时 body 区按容量
   分配、有效长度写进 attr tag 2）；其余一律 LAYOUT_LENGTH。NewScalar/NewTensor
   没有容量参数，恒为 LAYOUT_LENGTH。NewSlack/NewCode/NewReserve 以 cap > len 判定。

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
                              uint64_t cap, const char *langtype,
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
                             uint64_t cap, const char *langtype,
                             uint8_t **out, uint64_t *out_len);
/* 预留 = body 区 cap 字节零填；有效长度 body_len（cap > body_len 时 layout=1）。 */
int kvspaceXvalueHeadReserve(uint64_t body_len, uint64_t cap,
                             const char *langtype,
                             uint8_t **out, uint64_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
