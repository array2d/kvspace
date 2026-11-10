/* kvspace_xvalue.h — kvspace 存储层的 XValue **字节布局**。
 *
 * 这个头只管一件事：16 字节 head 与 body/attr 区怎么排、怎么读、怎么写。
 * 它**不知道 langtype 是什么**——`tag 1` 在这里只是一个 attr 记录的标签，
 * 内容对存储层是不透明的字节。
 *
 * 语义（langtype、指针前缀、数组形态、reallen 的含义）在 `kvlang_xvalue.h`。
 * 两者分开是有意的：kvspace 是存储层，kvlang 是语言层；把语言概念塞进存储
 * 头会让 kvspace 依赖它不该知道的东西，也会让「存储层可独立替换」失效。
 *
 * Wire: [bodylen:u64 LE@0][attrslen:u16 LE@8][padding:6@10-15]
 *       [body @16..16+bodylen][attr @16+bodylen..16+bodylen+attrslen]
 * Head 固定 16 字节，body 偏移恒为 16。
 * `bodylen` 恒为 body 区的**有效字节数**。容量是存储后端的物理分配属性，
 * 不进入 wire，也不改变 bodylen 的含义。
 * attr 区**没有**长度前缀：attrslen 在 head 里，再加一层前缀就是第二个真相。
 * 总长 = 16 + bodylen + attrslen，全部由 head 算出。
 *
 * record = [tag:u8][len:u16 LE][data len 字节]；record 之间紧密排列，末尾不 pad。
 * 本头只定义 tag 的**编号空间**，不解释任何 tag 的含义。
 *
 * Wire is little-endian. 当前平台都是 LE，所以结构体 memcpy 直接等于 wire；
 * 大端目标必须在边界处转换。 */
#ifndef KVSPACE_XVALUE_H
#define KVSPACE_XVALUE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* 公开符号的导出标注：生产构建用 `-fvisibility=hidden`，只有标了它的符号
 * 才会出现在动态符号表里。不标 = 内部实现（kvspaceShm* 等），测试构建
 * （不留 hidden）可直接链接。
 *
 * 定义在字节层头里是因为三个公开头都要用，而 kvspace_xvalue.h 是最底层、
 * 被另外两个包含的那一个。 */
#if defined(_WIN32)
#define KVSPACE_PUBLIC __declspec(dllexport)
#elif defined(__GNUC__)
#define KVSPACE_PUBLIC __attribute__((visibility("default")))
#else
#define KVSPACE_PUBLIC
#endif

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

/* 固定 16 字节 head（结构体直接 memcpy + body + attr）。
   wire: bodylen:u64@0, attrslen:u16@8, padding@10-15。 */
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
    const uint64_t body = kvspaceXvalueHeadLen(h);
    const uint64_t attrs = kvspaceXvalueHeadAttrsLen(h);
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

/* ── attr 记录：编号空间 ──────────────────────────────────────────────
 *
 * 这里只定义「tag 是 u8 编号」，不定义任何 tag 的语义——语义在 kvlang_xvalue.h。
 *
 * 唯一的例外是 tag 2：本头提供它的常量与拒绝规则。理由：「拒绝一个已退役的
 * 编号」属于**线格式契约**而不是语言语义——任何读写层拿到 tag 2 都必须拒，
 * 不管它懂不懂这些 tag 表达什么。 */
#define KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN 2u

/* attr 区唯一的一对读写入口。record 恒为 `[tag:u8][len:u16 LE][data]`。
 *
 * GetAttr：在 wire 的 attr 区里找 `tag`，找到就把 data 起址与长度写进输出。
 *   - 返回 0 = 找到；-1 = 没找到或 wire 畸形（输出清零）。
 *   - 找不到不是错误，是「这个 tag 没有」——调用方靠返回码区分，不靠 *out_data
 *     为 NULL 区分（data 长度可以为 0，此时 *out_data 非 NULL）。
 *   - padding 非零、总长越界、record 越界、tag 2 出现，一律 -1。
 *     tag 2 无论在问哪个 tag 都拒：它是「写读双方理解不一致」的证据。
 *
 * SetAttr：替换或追加一条记录。
 *   - tag 已存在 → 就地替换（长度可变，其后记录整体搬移）；
 *   - 不存在 → 追加到 attr 区末尾；
 *   - len == 0 且 data == NULL → **删除**该 tag（不存在则成功，幂等）；
 *   - wire_capacity 是整条 wire 的可用字节数，不足则失败且**不改动 wire**；
 *   - 成功时 *out_new_length 是新的整条 wire 长度（head 的 attrslen 已更新）。
 *   - 返回 0 成功，-1 失败（含容量不足、wire 畸形、tag == 2 或 tag == 0）。 */
int kvspaceXvalueGetAttr(const uint8_t *wire, uint64_t wire_len, uint8_t tag,
                          const uint8_t **out_data, uint16_t *out_len);
int kvspaceXvalueSetAttr(uint8_t *wire, uint64_t wire_capacity, uint8_t tag,
                          const uint8_t *data, uint16_t len,
                          uint64_t *out_new_length);

/* 有效长度 = bodylen。读之前确认整条 wire 自洽（padding 为零、总长不越界、
   attr 区无 tag 2），否则「长度」是从一条坏 wire 上读出来的，不该采信。
   返回 0 成功；-1 失败（*out 置 0，因为 0 本身是合法长度，错误不走长度值）。 */
int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire, uint64_t wire_len,
                                    uint64_t *out);

#ifdef __cplusplus
}
#endif

#endif
