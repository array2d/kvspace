/* 旧 `<kvspace/kvspace.h>` 已删除。本文件导出的 `kvspaceNew*` 系列是旧 ABI 的
 * 遗留符号（仍在 .so 里到 P4/P5），它们的声明随公共头一起消失，只能在这里
 * 私有维持一份。这不是「把删掉的头搬回来」——它只覆盖本 TU 自己要定义的函数。 */
#include <kvspace/kvspace_xvalue.h>
#include <kvspace/kvlang_xvalue.h>

#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int finish(uint8_t *value, uint64_t len, uint8_t **out, uint32_t *out_len) {
    if (!value || len > UINT32_MAX) {
        free(value);
        return 1;
    }
    *out = value;
    *out_len = (uint32_t)len;
    return 0;
}

KVSPACE_PUBLIC int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len)
        return 1;
    /* `any` 不是类型名（空 kindexpr = 不约束，见 spec [[文法与合法性]]）。 */
    if (strcmp(target_kindexpr, "any") == 0)
        return 1;
    /* 空 target 不是可用的指针目标（None 指针走 xvalue_head 构造器，不经本 codec）。 */
    if (*target == '\0')
        return 1;
    /* target_kindexpr 是被指向类型的基 kindexpr；`*` 前缀由 NewPtr 唯一补上。 */
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewPtr((const uint8_t *)target,
                                (uint32_t)strlen(target), target_kindexpr,
                                &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

KVSPACE_PUBLIC int kvspaceNewChar(const uint8_t *bytes, uint32_t len, const char *langtype,
                   uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || (len && !bytes))
        return 1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewSlack(bytes, len, langtype, &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

KVSPACE_PUBLIC int kvspaceNewBool(uint8_t v, const char *langtype,
                   uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    uint8_t b = v ? 1 : 0;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewScalar(&b, 1, langtype, &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

KVSPACE_PUBLIC int kvspaceNewInt64(int64_t v, const char *langtype,
                    uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)((uint64_t)v >> (8 * i));
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewScalar(raw, 8, langtype, &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

KVSPACE_PUBLIC int kvspaceNewFloat64(double v, const char *langtype,
                      uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)(bits >> (8 * i));
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewScalar(raw, 8, langtype, &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}
