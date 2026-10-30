#include <kvspace/kvspace.h>
#include <kvspace/xvalue_head.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

/* 固定 head 8 字节 [len:u48 LE@0-5][storetype@6][ptr@7]，body 偏移恒 8；
   langtype 不在 head（x‥langtype attr key）。构造直接 memcpy struct + body——
   New* 产物用 kvspaceXvalueHead 字段断言。 */

int main(void) {
    uint8_t *value = NULL;
    uint32_t len = 0;
    kvspaceXvalueHead h;

    CHECK(kvspaceNewInt64(42, &value, &len) == 0);
    CHECK(len == 16);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT && h.ptr == KVSPACE_XVALUE_PTR_INLINE &&
          kvspaceXvalueHeadLen(&h) == 8);
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | value[KVSPACE_XVALUE_HEAD_LEN + i];
    CHECK(v == 42);
    free(value);

    CHECK(kvspaceNewBool(1, &value, &len) == 0);
    CHECK(len == 9);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT &&
          kvspaceXvalueHeadLen(&h) == 1 && value[KVSPACE_XVALUE_HEAD_LEN] == 1);
    free(value);
    CHECK(kvspaceNewBool(0, &value, &len) == 0);
    CHECK(value[KVSPACE_XVALUE_HEAD_LEN] == 0);
    free(value);

    CHECK(kvspaceNewFloat64(2.5, &value, &len) == 0);
    CHECK(len == 16);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT &&
          kvspaceXvalueHeadLen(&h) == 8);
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--)
        bits = (bits << 8) | value[KVSPACE_XVALUE_HEAD_LEN + i];
    double d;
    memcpy(&d, &bits, sizeof d);
    CHECK(d == 2.5);
    free(value);

    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceNewChar(utf8, sizeof utf8, &value, &len) == 0);
    CHECK(len == 8 + sizeof utf8);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          h.ptr == KVSPACE_XVALUE_PTR_INLINE &&
          kvspaceXvalueHeadLen(&h) == sizeof utf8);
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    free(value);
    CHECK(kvspaceNewChar(NULL, 0, &value, &len) == 0);
    CHECK(len == 8);
    free(value);

    CHECK(kvspaceNewPtr("int64", "/x", &value, &len) == 0);
    CHECK(len == 10);
    memcpy(&h, value, sizeof h);
    CHECK(h.ptr == KVSPACE_XVALUE_PTR_PTR && h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&h) == 2);
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, "/x", 2) == 0);
    free(value);
    /* 空 kindexpr = 不约束：Ptr 目标允许为空；`any` 不是类型名，创建即拒。 */
    CHECK(kvspaceNewPtr("", "/x", &value, &len) == 0);
    CHECK(len == 10);
    memcpy(&h, value, sizeof h);
    CHECK(h.ptr == KVSPACE_XVALUE_PTR_PTR && kvspaceXvalueHeadLen(&h) == 2);
    free(value);
    CHECK(kvspaceNewPtr("any", "/x", &value, &len) != 0);

    puts("ok");
    return 0;
}
