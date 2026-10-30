#include "kvspace/kvspace.h"
#include <kvspace/xvalue_head.h>

#include <limits.h>
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

int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len) {
    if (!target_kindexpr || !target || !out || !out_len)
        return 1;
    /* `any` 不是类型名（空 kindexpr = 不约束，见 spec [[文法与合法性]]）。 */
    if (strcmp(target_kindexpr, "any") == 0)
        return 1;
    /* target_kindexpr 写入 x‥langtype attr key（由调用方/GetHead 层处理）；
       head 本体固定，只存指针路径。 */
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewPtr((const uint8_t *)target,
                                (uint32_t)strlen(target), &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len || (len && !bytes))
        return 1;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewSlack(bytes, len, len, &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    uint8_t b = v ? 1 : 0;
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, &b, 1,
                                   &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)((uint64_t)v >> (8 * i));
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8,
                                   &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}

int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len) {
    if (!out || !out_len)
        return 1;
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    uint8_t raw[8];
    for (int i = 0; i < 8; i++)
        raw[i] = (uint8_t)(bits >> (8 * i));
    uint8_t *value = NULL;
    uint64_t total = 0;
    if (kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8,
                                   &value, &total) != 0)
        return 1;
    return finish(value, total, out, out_len);
}
