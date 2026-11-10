/* 测试造值用的最小 wire 构造器。
 *
 * 为什么测试自己造而不是链 kvlang 的 `libkvlang_xvalue.a`：**kvspace 不得依赖
 * kvlang**。哪怕只是测试依赖，构建图里也会出现 kvspace → kvlang 的箭头，而分层
 * 的方向是单向的（语言层依赖存储层）。
 *
 * 这不是「复制实现」：这些构造器只拼一条 wire —— `[bodylen:u64][attrslen:u16]
 * [padding:6][body][attr]`，attr 只有一条可选记录 `[1][len:u16][langtype]`。
 * 格式知识在这里重复是**有意的**：它由同一组测试矩阵约束（含与 kvlang 金标准的
 * 逐字节比对），而共享符号会把两个仓编译期绑死。
 *
 * 头里的 inline 访问器（`kvspaceXvalueHeadSetLen` 等）来自 kvspace 自己的字节层头，
 * 不算语言层依赖。 */
#ifndef KVSPACE_TEST_WIRE_H
#define KVSPACE_TEST_WIRE_H

#include <kvspace/kvspace_xvalue.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* attr 记录的 tag：1 = langtype。存储层不知道它的语义，测试造值时知道。 */
#define KVSPACE_TEST_ATTR_TAG_LANGTYPE 1u

/* 拼一条 wire。`prefix` 非零时写在 langtype 前面（'*' 指针 / '@' 外部引用）。
 * body 为 NULL 且 body_len > 0 时按零填（对应 Reserve 语义）。
 * 成功返回 0，`*out` 由调用方 free。 */
static inline int kvspace_test_wire_build(const uint8_t *body, uint64_t body_len,
                                          int prefix, const char *langtype,
                                          uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return -1;
    *out = NULL;
    *out_len = 0;
    if (body_len && !body && prefix == 0)
        return -1;
    const uint64_t langtype_len = langtype ? (uint64_t)strlen(langtype) : 0;
    const uint64_t record_len = langtype_len + (prefix ? 1u : 0u);
    const uint64_t attrs_len = record_len ? 3u + record_len : 0u;
    if (attrs_len > UINT16_MAX)
        return -1;
    const uint64_t total = KVSPACE_XVALUE_HEAD_LEN + body_len + attrs_len;
    uint8_t *wire = (uint8_t *)calloc(1, (size_t)total);
    if (!wire)
        return -1;
    kvspaceXvalueHead head = {.padding = {0u, 0u, 0u, 0u, 0u, 0u}};
    kvspaceXvalueHeadSetLen(&head, body_len);
    kvspaceXvalueHeadSetAttrsLen(&head, (uint16_t)attrs_len);
    memcpy(wire, &head, sizeof head);
    if (body && body_len)
        memcpy(wire + KVSPACE_XVALUE_HEAD_LEN, body, (size_t)body_len);
    if (record_len) {
        uint8_t *record = wire + KVSPACE_XVALUE_HEAD_LEN + body_len;
        record[0] = KVSPACE_TEST_ATTR_TAG_LANGTYPE;
        record[1] = (uint8_t)record_len;
        record[2] = (uint8_t)(record_len >> 8);
        uint64_t at = 3;
        if (prefix)
            record[at++] = (uint8_t)prefix;
        if (langtype_len)
            memcpy(record + at, langtype, (size_t)langtype_len);
    }
    *out = wire;
    *out_len = total;
    return 0;
}

/* 标量：定长 body + 可选 langtype。 */
static inline int kvspace_test_wire_scalar(const uint8_t *raw, uint32_t raw_len,
                                           const char *langtype, uint8_t **out,
                                           uint64_t *out_len) {
    if (raw_len && !raw) {
        if (out)
            *out = NULL;
        return -1;
    }
    return kvspace_test_wire_build(raw, raw_len, 0, langtype, out, out_len);
}

/* 变长：body 字节数由调用方给全（无容量概念）。 */
static inline int kvspace_test_wire_slack(const uint8_t *data, uint64_t data_len,
                                          const char *langtype, uint8_t **out,
                                          uint64_t *out_len) {
    if (data_len && !data) {
        if (out)
            *out = NULL;
        return -1;
    }
    return kvspace_test_wire_build(data, data_len, 0, langtype, out, out_len);
}

/* 指针：`*` 前缀进 langtype 记录。空目标 = None 指针（合法）。 */
static inline int kvspace_test_wire_ptr(const uint8_t *path, uint32_t path_len,
                                        const char *langtype, uint8_t **out,
                                        uint64_t *out_len) {
    if (path_len && !path) {
        if (out)
            *out = NULL;
        return -1;
    }
    if (langtype && (langtype[0] == '*' || langtype[0] == '@')) {
        if (out)
            *out = NULL;
        return -1;
    }
    return kvspace_test_wire_build(path, path_len, '*', langtype, out, out_len);
}

/* 外部引用：`@` 前缀；locator 不得为空。 */
static inline int kvspace_test_wire_ext(const uint8_t *locator,
                                        uint32_t locator_len,
                                        const char *langtype, uint8_t **out,
                                        uint64_t *out_len) {
    if (!locator_len || !locator) {
        if (out)
            *out = NULL;
        return -1;
    }
    if (langtype && (langtype[0] == '*' || langtype[0] == '@')) {
        if (out)
            *out = NULL;
        return -1;
    }
    return kvspace_test_wire_build(locator, locator_len, '@', langtype, out,
                                   out_len);
}

#endif
