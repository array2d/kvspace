#include <kvspace/xvalue_head.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 固定 head 16 字节 [bodylen:u64@0][attrslen:u32@8][storetype@12][ptr@13][pad@14]，
   body 偏移恒 16，attr 区紧随 body 且**无**自带长度前缀（长度只在 head 里）。
   单 key 整体——无 attr key。 */

static int failures = 0;
#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            failures++;                                                        \
            fprintf(stderr, "  FAIL %s:%d: ", __FILE__, __LINE__);             \
            fprintf(stderr, __VA_ARGS__);                                      \
            fprintf(stderr, "\n");                                             \
        }                                                                      \
    } while (0)

/* 校验 head 与 attr 区：head 的 bodylen/attrslen/padding 正确，
   total = 16+bodylen+attrslen；langtype 记录存在且与 lt 一致。 */
static int attr_ok(const uint8_t *v, uint64_t total, uint64_t body_len,
                   const char *lt) {
    size_t l = strlen(lt);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    if (kvspaceXvalueHeadLen(&h) != body_len || !kvspaceXvalueHeadPaddingIsZero(&h))
        return 0;
    const uint64_t alen = kvspaceXvalueHeadAttrsLen(&h);
    if (total != KVSPACE_XVALUE_HEAD_LEN + body_len + alen)
        return 0;
    const uint8_t *a = v + KVSPACE_XVALUE_HEAD_LEN + body_len;
    if (l == 0)
        return alen == 0;
    if (alen != 3 + l)
        return 0;
    return a[0] == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE && a[1] == l && a[2] == 0 &&
           memcmp(a + 3, lt, l) == 0;
}

static void test_none(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&buf, &n) == 0, "none new");
    CHECK(n == 16, "none len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SHORT &&
          h.ptr == KVSPACE_XVALUE_PTR_INLINE && kvspaceXvalueHeadLen(&h) == 0, "none head");
    /* wire 布局：bodylen:u64@0、attrslen:u32@8、storetype@12、ptr@13、pad@14 */
    CHECK(buf[0] == 0 && buf[7] == 0 && buf[8] == 0 && buf[11] == 0 &&
          buf[12] == KVSPACE_XVALUE_ST_SHORT &&
          buf[13] == KVSPACE_XVALUE_PTR_INLINE && buf[14] == 0 && buf[15] == 0,
          "none wire layout");
    CHECK(attr_ok(buf, n, 0, ""), "none empty attr");
    free(buf);
}

static void test_scalar(void) {
    uint8_t raw[8] = {0x0a, 0, 0, 0, 0, 0, 0, 0};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "int64",
                                     &buf, &n) == 0, "i64 new");
    CHECK(n == 32, "i64 len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8 && h.storetype == KVSPACE_XVALUE_ST_SHORT, "i64 head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0, "i64 body@8");
    CHECK(attr_ok(buf, n, 8, "int64"), "i64 langtype attr");
    free(buf);
    /* 无类型（空 langtype）：attr 区为空，attrslen = 0。 */
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw, 8, "",
                                     &buf, &n) == 0, "scalar no-langtype");
    CHECK(n == 24, "no-langtype len %llu", (unsigned long long)n);
    CHECK(attr_ok(buf, n, 8, ""), "no-langtype empty attr");
    free(buf);
}

static void test_slack(void) {
    const uint8_t hi[] = {'h', 'i'};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, 2, "[]char/utf8", &buf, &n) == 0, "slack new");
    CHECK(n == 32, "slack len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&h) == 2, "slack head");
    /* wire：bodylen u64 LE@0=2，storetype@12，ptr@13，pad@14-15 为零 */
    CHECK(buf[0] == 2 && buf[1] == 0 && buf[7] == 0 &&
          buf[12] == KVSPACE_XVALUE_ST_SLACK &&
          buf[13] == KVSPACE_XVALUE_PTR_INLINE && buf[14] == 0 && buf[15] == 0,
          "slack wire layout");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, hi, 2) == 0, "slack body");
    CHECK(attr_ok(buf, n, 2, "[]char/utf8"), "slack langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, 1, "", &buf, &n) != 0, "slack cap<len");
}

static void test_tensor(void) {
    uint64_t dims[2] = {2, 3};
    uint8_t raw[24];
    memset(raw, 0xab, sizeof raw);
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, raw, sizeof raw, "[2,3]int64",
                                     &buf, &n) == 0, "tensor new");
    CHECK(n == 53, "tensor len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_LARGE &&
          kvspaceXvalueHeadLen(&h) == 24, "tensor head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 24) == 0, "tensor body@8");
    CHECK(attr_ok(buf, n, 24, "[2,3]int64"), "tensor langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewTensor(NULL, 0, raw, 24, "", &buf, &n) != 0,
          "tensor no dims");
}

static void test_ptr_ext(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/varx", 5, "int64",
                                  &buf, &n) == 0, "ptr new");
    CHECK(n == 16 + 5 + 8, "ptr len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          h.ptr == KVSPACE_XVALUE_PTR_PTR &&
          kvspaceXvalueHeadLen(&h) == 5, "ptr head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, "/varx", 5) == 0, "ptr body");
    CHECK(attr_ok(buf, n, 5, "int64"), "ptr langtype attr");
    free(buf);

    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, "",
                                  &buf, &n) == 0, "ext new");
    memcpy(&h, buf, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_EXT &&
          h.ptr == KVSPACE_XVALUE_PTR_EXT, "ext head");
    CHECK(attr_ok(buf, n, 5, ""), "ext empty attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 0, "", &buf, &n) != 0, "empty ptr");
}

static void test_reserve_code(void) {
    uint8_t *value = NULL;
    uint64_t len = 0;
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 7, 10, "[]u8",
                                   &value, &len) == 0, "reserve slack");
    CHECK(len == 30, "reserve len");
    kvspaceXvalueHead h;
    memcpy(&h, value, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 7, "reserve head len");
    CHECK(attr_ok(value, len, 7, "[]u8"), "reserve langtype attr");
    const uint8_t utf8[] = {0xe4, 0xbd, 0xa0, 0xf0, 0x9f, 0x98, 0x80};
    memcpy(value + KVSPACE_XVALUE_HEAD_LEN, utf8, sizeof utf8);
    free(value);
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_LARGE, 10, 5, "",
                                   &value, &len) != 0, "reserve body>cap");

    const uint8_t call_body[] = {0, 0, 0, 0, 0, 'f'};
    CHECK(kvspaceXvalueHeadNewCode(call_body, sizeof call_body, sizeof call_body,
                                   "rwir", &value, &len) == 0, "code new");
    memcpy(&h, value, sizeof h);
    CHECK(h.storetype == KVSPACE_XVALUE_ST_SLACK &&
          kvspaceXvalueHeadLen(&h) == sizeof call_body,
          "code head");
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, call_body,
                 sizeof call_body) == 0, "code body");
    CHECK(attr_ok(value, len, sizeof call_body, "rwir"), "code langtype attr");
    free(value);
    CHECK(kvspaceXvalueHeadNewCode(call_body, 5, 3, "", &value, &len) != 0,
          "code cap<len");
}

/* Failure contract and bounded langtype parsing:
   - All constructors return -1 and clear *out = NULL on failure;
   - Reserve validates storetype via build (rejecting > KVSPACE_XVALUE_ST_EXT);
   - LangtypeBounded returns the exact non-NUL-terminated slice for valid values
     and 0 with *out = NULL on truncated/malformed inputs without reading out of bounds. */
static void test_bounded_and_reserve_fail(void) {
    uint8_t sentinel = 0x5a;
    uint8_t *buf = &sentinel;
    uint64_t n = 0;
    const uint8_t *lt = &sentinel;

    /* Reserve with invalid storetype (4, 9, 255) or cap < body_len: reject and clear *out */
    CHECK(kvspaceXvalueHeadReserve(4, 4, 4, "int32", &buf, &n) == -1 && buf == NULL,
          "Reserve(storetype=4) rejected and clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(9, 4, 4, "int32", &buf, &n) == -1 && buf == NULL,
          "Reserve(storetype=9) rejected and clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(255, 0, 0, "", &buf, &n) == -1 && buf == NULL,
          "Reserve(storetype=255) rejected and clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 8, 4, "", &buf, &n) == -1 &&
              buf == NULL,
          "Reserve(cap < body_len) rejected and clears *out");
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 4, 4, "", NULL, &n) == -1,
          "Reserve(out=NULL) rejected");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 4, 4, "", &buf, NULL) == -1 &&
              buf == NULL,
          "Reserve(out_len=NULL) rejected and clears *out");

    /* Other constructors also clear *out on failure */
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, NULL, 3, "", &buf, &n) == -1 &&
              buf == NULL,
          "NewScalar(NULL, 3) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewScalar(4, &sentinel, 1, "", &buf, &n) == -1 && buf == NULL,
          "NewScalar(storetype=4) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewSlack(NULL, 4, 4, "", &buf, &n) == -1 && buf == NULL,
          "NewSlack(NULL, 4) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewTensor(NULL, 0, &sentinel, 1, "", &buf, &n) == -1 && buf == NULL,
          "NewTensor(NULL dims) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 0, "", &buf, &n) == -1 && buf == NULL,
          "NewPtr(empty) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewExt(NULL, 0, "", &buf, &n) == -1 && buf == NULL,
          "NewExt(empty) clears *out");

    /* LangtypeBounded valid inputs: scalar / tensor / ptr / empty langtype */
    uint8_t raw8[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT, raw8, 8, "int64", &buf, &n) == 0,
          "scalar build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 5 && lt != NULL &&
              memcmp(lt, "int64", 5) == 0,
          "bounded scalar langtype");
    free(buf);

    uint64_t dims[2] = {2, 3};
    uint8_t raw24[24] = {0};
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, raw24, sizeof raw24, "[2,3]float32", &buf, &n) == 0,
          "tensor build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 12 && lt != NULL &&
              memcmp(lt, "[2,3]float32", 12) == 0,
          "bounded tensor langtype");
    free(buf);

    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/dst", 4, "[]char/utf8", &buf, &n) == 0,
          "ptr build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 11 && lt != NULL &&
              memcmp(lt, "[]char/utf8", 11) == 0,
          "bounded ptr langtype");
    free(buf);

    CHECK(kvspaceXvalueHeadNewNone(&buf, &n) == 0, "none build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 0 && lt == NULL,
          "bounded empty langtype returns 0 and *out=NULL");
    free(buf);

    /* LangtypeBounded malformed / truncated boundary cases */
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(NULL, 16, &lt) == 0 && lt == NULL,
          "bounded NULL wire");

    uint8_t short_wire[9] = {0};
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(short_wire, 0, &lt) == 0 && lt == NULL,
          "bounded wire_len=0");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(short_wire, 9, &lt) == 0 && lt == NULL,
          "bounded wire_len=9 (< 10)");

    /* 截断的 wire：head 说 body+attrs 比实际字节多（head 自己就必须能挡住）。 */
    CHECK(kvspaceXvalueHeadReserve(KVSPACE_XVALUE_ST_SLACK, 4, 4, "int32", &buf, &n) == 0,
          "reserve for truncation checks");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, KVSPACE_XVALUE_HEAD_LEN + 4, &lt) == 0 &&
              lt == NULL,
          "bounded rejects wire cut before the attr region");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, KVSPACE_XVALUE_HEAD_LEN + 4 + 1, &lt) == 0 &&
              lt == NULL,
          "bounded rejects wire cut one byte into the attr region");
    /* attrslen 超出剩余字节（掐掉最后一个字节） */
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n - 1, &lt) == 0 && lt == NULL,
          "bounded rejects attrslen exceeding the remaining bytes");
    free(buf);

    /* attrslen 放不下一个 record 头（< 3）：无法解析即失败。 */
    uint8_t bad_alen[16 + 1] = {0};
    bad_alen[8] = 1;
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(bad_alen, sizeof bad_alen, &lt) == 0 &&
              lt == NULL,
          "bounded rejects attrslen smaller than a record header");

    /* Truncated record: attrslen fits in wire_len, but p + 3 + rlen > end */
    uint8_t trunc_rec[16 + 7] = {
        0, 0, 0, 0, 0, 0, 0, 0,             /* head: bodylen=0 */
        7, 0, 0, 0,                         /* attrslen=7 (end = 16+7) */
        0, 0, 0, 0,                         /* storetype/ptr/padding */
        KVSPACE_XVALUE_ATTR_TAG_LANGTYPE,   /* tag = 1 */
        10, 0,                              /* rlen = 10 -> p+3+10 = 29 > 23 */
        'i', '6'
    };
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(trunc_rec, sizeof trunc_rec, &lt) == 0 && lt == NULL,
          "bounded rejects truncated attr record (p + 3 + rlen > end)");

    /* Trailing record header shorter than 3 bytes (attrslen = 4 leaves 2 bytes) */
    uint8_t short_rec_hdr[16 + 2] = {
        0, 0, 0, 0, 0, 0, 0, 0,             /* bodylen=0 */
        2, 0, 0, 0,                          /* attrslen=2 -> p+3 > end */
        0, 0, 0, 0,
        KVSPACE_XVALUE_ATTR_TAG_LANGTYPE, 0
    };
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(short_rec_hdr, sizeof short_rec_hdr, &lt) == 0 &&
              lt == NULL,
          "bounded rejects <3B trailing record header");

    /* Non-NUL-terminated bounded slice after skipping an unknown tag:
       record 1 is tag=2 (reserved), record 2 is tag=1 (langtype="int64") immediately
       followed by non-NUL byte 9 (next record tag); verify exact rlen=5 slice. */
    uint8_t multi_non_nul[16 + (3 + 2) + (3 + 5) + (3 + 1)] = {
        0, 0, 0, 0, 0, 0, 0, 0,             /* bodylen=0 */
        17, 0, 0, 0,                         /* attrslen = 5 + 8 + 4 = 17 */
        0, 0, 0, 0,
        2, 2, 0, 0xaa, 0xbb,                /* tag=2, rlen=2 */
        KVSPACE_XVALUE_ATTR_TAG_LANGTYPE, 5, 0, 'i', 'n', 't', '6', '4', /* tag=1, rlen=5 */
        9, 1, 0, 'Z'                        /* tag=9, rlen=1 -> lt[5] == 9 != '\0' */
    };
    lt = &sentinel;
    uint16_t got_len = kvspaceXvalueHeadLangtypeBounded(multi_non_nul, sizeof multi_non_nul, &lt);
    CHECK(got_len == 5 && lt != NULL && lt[5] == 9 && memcmp(lt, "int64", 5) == 0,
          "bounded returns exact non-NUL-terminated slice after skipping unknown tag");
}

/* padding 是 ABI 的一部分：非零即非法，读取路径必须先过这一关。 */
static void test_nonzero_padding_is_rejected(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(KVSPACE_XVALUE_ST_SHORT,
                                     (const uint8_t *)"abcd", 4, "int64",
                                     &buf, &n) == 0, "pad build");
    const uint8_t *lt = (const uint8_t *)"sentinel";
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 5 && lt != NULL,
          "pad zero accepted");
    buf[14] = 1; /* 破坏 padding */
    lt = (const uint8_t *)"sentinel";
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 0 && lt == NULL,
          "nonzero padding rejected by bounded reader");
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(!kvspaceXvalueHeadPaddingIsZero(&h), "helper sees nonzero padding");
    CHECK(kvspaceXvalueHeadTotalLen(&h) == 0, "total len refuses nonzero padding");
    buf[14] = 0;
    buf[15] = 1;
    lt = (const uint8_t *)"sentinel";
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 0 && lt == NULL,
          "nonzero padding byte 2 rejected");
    free(buf);
}

/* head 单独就能算出总长：不需要先扫 body 之后的前缀。 */
static void test_total_len_from_head_only(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewSlack((const uint8_t *)"xyz", 3, 3, "[]char/utf8",
                                    &buf, &n) == 0, "total build");
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadTotalLen(&h) == n, "total len equals built length");
    CHECK(kvspaceXvalueHeadAttrsLen(&h) == 3 + 11, "attrslen excludes any prefix");
    CHECK(kvspaceXvalueHeadAttrs(buf) == buf + KVSPACE_XVALUE_HEAD_LEN + 3,
          "attrs start right after body");
    free(buf);
}

int main(void) {
    test_nonzero_padding_is_rejected();
    test_total_len_from_head_only();
    test_none();
    test_scalar();
    test_slack();
    test_tensor();
    test_ptr_ext();
    test_reserve_code();
    test_bounded_and_reserve_fail();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
