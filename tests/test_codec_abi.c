#include <kvspace/kvspace.h>
#include <kvspace/xvalue_head.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "failed: %s\n", #x); return 1; } } while (0)

/* 固定 head 8 字节 [len:u48 LE@0-5][storetype@6][ptr@7]，body 偏移恒 8，
   attr 区 @8+len（attr_len:u16 自描述，含 langtype 记录）。单 key 整体。 */

/* 校验 attr 区：total = 8+body_len+attr_len；langtype 记录存在且与 lt 一致。 */
static int attr_ok(const uint8_t *value, uint32_t len, uint64_t body_len,
                   const char *langtype) {
    size_t lt = strlen(langtype);
    const uint8_t *a = value + KVSPACE_XVALUE_HEAD_LEN + body_len;
    uint64_t alen = (uint64_t)a[0] | ((uint64_t)a[1] << 8);
    if ((uint64_t)len != KVSPACE_XVALUE_HEAD_LEN + body_len + alen)
        return 0;
    if (lt == 0)
        return alen == 2;
    if (alen != 2 + 3 + lt)
        return 0;
    return a[2] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[3] == lt && a[4] == 0 &&
           memcmp(a + 5, langtype, lt) == 0;
}

int main(void) {
    uint8_t *value = NULL;
    uint32_t len = 0;
    kvspaceXvalueHead h;

    CHECK(kvspaceNewInt64(42, "int64", &value, &len) == 0);
    CHECK(len == 26);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT && h.ptr == KVSPACE_XVALUE_PTR_INLINE &&
          kvspaceXvalueHeadLen(&h) == 8);
    CHECK(attr_ok(value, len, 8, "int64"));
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | value[KVSPACE_XVALUE_HEAD_LEN + i];
    CHECK(v == 42);
    free(value);

    CHECK(kvspaceNewBool(1, "bool", &value, &len) == 0);
    CHECK(len == 18);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT &&
          kvspaceXvalueHeadLen(&h) == 1 && value[KVSPACE_XVALUE_HEAD_LEN] == 1);
    CHECK(attr_ok(value, len, 1, "bool"));
    free(value);
    CHECK(kvspaceNewBool(0, "bool", &value, &len) == 0);
    CHECK(value[KVSPACE_XVALUE_HEAD_LEN] == 0);
    free(value);

    CHECK(kvspaceNewFloat64(2.5, "float64", &value, &len) == 0);
    CHECK(len == 28);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT &&
          kvspaceXvalueHeadLen(&h) == 8);
    CHECK(attr_ok(value, len, 8, "float64"));
    uint64_t bits = 0;
    for (int i = 7; i >= 0; i--)
        bits = (bits << 8) | value[KVSPACE_XVALUE_HEAD_LEN + i];
    double d;
    memcpy(&d, &bits, sizeof d);
    CHECK(d == 2.5);
    free(value);

    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    CHECK(kvspaceNewChar(utf8, sizeof utf8, "char/utf8", &value, &len) == 0);
    CHECK(len == 8 + sizeof utf8 + 14);
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          h.ptr == KVSPACE_XVALUE_PTR_INLINE &&
          kvspaceXvalueHeadLen(&h) == sizeof utf8);
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8) == 0);
    CHECK(attr_ok(value, len, sizeof utf8, "char/utf8"));
    free(value);
    CHECK(kvspaceNewChar(NULL, 0, "", &value, &len) == 0);
    CHECK(len == 10);
    free(value);

    CHECK(kvspaceNewPtr("int64", "/x", &value, &len) == 0);
    CHECK(len == 20);
    memcpy(&h, value, sizeof h);
    CHECK(h.ptr == KVSPACE_XVALUE_PTR_PTR && h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&h) == 2);
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, "/x", 2) == 0);
    CHECK(attr_ok(value, len, 2, "int64"));
    free(value);
    /* 空 kindexpr = 不约束：Ptr 目标允许为空；`any` 不是类型名，创建即拒。 */
    CHECK(kvspaceNewPtr("", "/x", &value, &len) == 0);
    CHECK(len == 12);
    memcpy(&h, value, sizeof h);
    CHECK(h.ptr == KVSPACE_XVALUE_PTR_PTR && kvspaceXvalueHeadLen(&h) == 2);
    free(value);
    CHECK(kvspaceNewPtr("any", "/x", &value, &len) != 0);

    puts("ok");
    return 0;
}
