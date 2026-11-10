/* kvlang_xvalue.h — kvlang 语言层对 XValue 的语义约定。
 *
 * `kvspace_xvalue.h` 只认字节：head、body、attr 记录，tag 只是编号。这个头补上
 * **语言才知道的那部分**：
 *
 *   - tag 1 = **langtype**：UTF-8 明文，含 `[dims]` 形状段与 `*`/`@` 前缀，
 *     是这个值的类型；
 *   - tag 2 = 已删除的 reallen（出现即拒）；
 *   - 构造器的名字与语义（None/Scalar/Slack/Tensor/Ptr/Ext/Code/Reserve）。
 *
 * 为什么必须分两个头：kvspace 是存储层，它不该知道「langtype」这个词。把语言
 * 概念写进存储头，等于让存储层依赖语言层——那样的 kvspace 就不能被别的语言
 * 复用，也不能被独立替换。反过来，语言层依赖存储层的字节布局是合理的：
 * langtype 就住在 attr 记录里。
 *
 * 分层的硬边界：**本头不定义任何字节偏移**。所有偏移、长度、越界检查都在
 * kvspace_xvalue.h 里做；这里只把「tag 1 是什么」翻译成调用能懂的形式。 */

#ifndef KVSPACE_KVLANG_XVALUE_H
#define KVSPACE_KVLANG_XVALUE_H

#include <kvspace/kvspace_xvalue.h>

#ifdef __cplusplus
extern "C" {
#endif

/* langtype 的 tag 编号。它定义在语言层：存储层只知道「attr 里有若干条记录」，
 * 不知道哪一条是类型。 */
#define KVSPACE_XVALUE_ATTR_TAG_LANGTYPE 1u

/* langtype 便捷包装：等价于 GetAttr(tag = 1)，只是少了 tag 参数。
   Unbounded 版假定调用方已确保 wire 够长（head 自己会校验总长）；
   Bounded 版先验 wire_len。两者都返回字节数，取不到返回 0 且 *out = NULL。 */
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
    if (kvspaceXvalueGetAttr(wire, total, KVSPACE_XVALUE_ATTR_TAG_LANGTYPE,
                             &langtype, &langtype_len) != 0)
        return 0;
    if (out)
        *out = langtype;
    return langtype_len;
}

/* Bounded variant: 先验 wire_len 再扫记录。
   取不到（缺失/畸形/截断）返回 0 且 *out = NULL。 */
static inline uint16_t kvspaceXvalueHeadLangtypeBounded(const uint8_t *wire,
                                                        uint64_t wire_len,
                                                        const uint8_t **out) {
    if (out)
        *out = NULL;
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    if (kvspaceXvalueGetAttr(wire, wire_len, KVSPACE_XVALUE_ATTR_TAG_LANGTYPE,
                             &langtype, &langtype_len) != 0)
        return 0;
    if (out)
        *out = langtype;
    return langtype_len;
}

/* 构造器（16 字节 head + body + attr 区，一条完整 wire）。
   空/NULL langtype 省略 langtype 记录（attrslen = 0）。
   成功返回 0；失败返回 -1 并清空 *out（out 非 NULL 时）。

   `bodylen` 恒为有效字节数；容量是存储后端的物理分配属性，不进入 wire。
   构造器没有 capacity 参数，也不写 attr tag 2。

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
