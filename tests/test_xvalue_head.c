#include <kvspace/xvalue_head.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 固定 head 16 字节 [bodylen:u64@0][attrslen:u16@8][reserved:u16@10]
   [layout@12][padding:3@13-15]，body 偏移恒 16，attr 区紧随 body 且**无**自带
   长度前缀（长度只在 head 里）。单 key 整体——无 attr key。
   head 不含 storetype/ptr：引用性在 langtype 前缀。 */

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

/* 校验 head 与 attr 区：region_len 是 body 区字节数（layout=1 时为容量），
   effective_len 是有效长度；total = 16+region_len+attrslen；
   langtype 记录存在且与 lt 一致（lt 含 * / @ 前缀）。 */
static int value_ok(const uint8_t *v, uint64_t total, uint8_t layout,
                    uint64_t region_len, uint64_t effective_len,
                    const char *lt) {
    size_t want = strlen(lt);
    kvspaceXvalueHead h;
    memcpy(&h, v, sizeof h);
    if (kvspaceXvalueHeadLen(&h) != region_len ||
        kvspaceXvalueHeadLayout(&h) != layout ||
        !kvspaceXvalueHeadPaddingIsZero(&h))
        return 0;
    if (kvspaceXvalueHeadTotalLen(&h) != total)
        return 0;
    const uint64_t alen = kvspaceXvalueHeadAttrsLen(&h);
    if (total != KVSPACE_XVALUE_HEAD_LEN + region_len + alen)
        return 0;
    uint64_t eff = UINT64_MAX;
    if (kvspaceXvalueHeadEffectiveLen(v, total, &eff) != 0 || eff != effective_len)
        return 0;
    const uint8_t *ltp = NULL;
    uint16_t n = kvspaceXvalueHeadLangtypeBounded(v, total, &ltp);
    if (n != want || (want && (!ltp || memcmp(ltp, lt, want) != 0)))
        return 0;
    /* 构造器把 langtype 记录放在最前；layout=1 时 tag 2 紧随其后。 */
    const uint8_t *a = v + KVSPACE_XVALUE_HEAD_LEN + region_len;
    if (want) {
        if (a[0] != KVSPACE_XVALUE_ATTR_TAG_LANGTYPE || a[1] != want ||
            a[2] != 0)
            return 0;
        a += 3 + want;
    }
    if (layout == KVSPACE_XVALUE_LAYOUT_CAPACITY) {
        if (a[0] != KVSPACE_XVALUE_ATTR_TAG_REALLEN || a[1] != 8 || a[2] != 0)
            return 0;
        uint64_t reallen = 0;
        for (int i = 0; i < 8; i++)
            reallen |= (uint64_t)a[3 + i] << (8 * i);
        if (reallen != effective_len)
            return 0;
    }
    return 1;
}

static void test_none(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewNone(&buf, &n) == 0, "none new");
    CHECK(n == 16, "none len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH &&
          kvspaceXvalueHeadLen(&h) == 0, "none head");
    /* wire 布局：bodylen@0-7、attrslen@8-9、reserved@10-11、layout@12、
       padding@13-15 全零 */
    CHECK(buf[0] == 0 && buf[7] == 0 && buf[8] == 0 && buf[9] == 0 &&
          buf[10] == 0 && buf[11] == 0 &&
          buf[12] == KVSPACE_XVALUE_LAYOUT_LENGTH && buf[13] == 0 &&
          buf[14] == 0 && buf[15] == 0,
          "none wire layout");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 0, 0, ""),
          "none empty attr");
    free(buf);
}

static void test_scalar(void) {
    uint8_t raw[8] = {0x0a, 0, 0, 0, 0, 0, 0, 0};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "int64", &buf, &n) == 0, "i64 new");
    CHECK(n == 32, "i64 len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadLen(&h) == 8 &&
          kvspaceXvalueHeadLayout(&h) == KVSPACE_XVALUE_LAYOUT_LENGTH,
          "i64 head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 8) == 0, "i64 body@16");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 8, 8, "int64"),
          "i64 langtype attr");
    free(buf);
    /* 无类型（空 langtype）：attr 区为空，attrslen = 0。 */
    CHECK(kvspaceXvalueHeadNewScalar(raw, 8, "", &buf, &n) == 0,
          "scalar no-langtype");
    CHECK(n == 24, "no-langtype len %llu", (unsigned long long)n);
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 8, 8, ""),
          "no-langtype empty attr");
    free(buf);
}

static void test_slack(void) {
    const uint8_t hi[] = {'h', 'i'};
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, 2, "[]char/utf8", &buf, &n) == 0,
          "slack new");
    CHECK(n == 32, "slack len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH &&
          kvspaceXvalueHeadLen(&h) == 2, "slack head");
    CHECK(buf[0] == 2 && buf[1] == 0 && buf[7] == 0 &&
          buf[12] == KVSPACE_XVALUE_LAYOUT_LENGTH && buf[13] == 0 &&
          buf[14] == 0 && buf[15] == 0,
          "slack wire layout");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, hi, 2) == 0, "slack body");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 2, 2, "[]char/utf8"),
          "slack langtype attr");
    free(buf);

    /* 容量 > 有效长度：layout=1，bodylen=容量 8，reallen=2，余量零填。 */
    CHECK(kvspaceXvalueHeadNewSlack(hi, 2, 8, "[]char/utf8", &buf, &n) == 0,
          "slack capacity new");
    CHECK(n == 49, "slack capacity len %llu", (unsigned long long)n);
    memcpy(&h, buf, sizeof h);
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_CAPACITY &&
          kvspaceXvalueHeadLen(&h) == 8, "slack capacity head");
    CHECK(buf[0] == 8 && buf[12] == KVSPACE_XVALUE_LAYOUT_CAPACITY &&
          buf[13] == 0 && buf[14] == 0 && buf[15] == 0,
          "slack capacity wire layout");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, hi, 2) == 0 &&
          buf[KVSPACE_XVALUE_HEAD_LEN + 2] == 0 &&
          buf[KVSPACE_XVALUE_HEAD_LEN + 7] == 0,
          "slack capacity body + zero fill");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_CAPACITY, 8, 2, "[]char/utf8"),
          "slack capacity attrs");
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
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH &&
          kvspaceXvalueHeadLen(&h) == 24, "tensor head");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, raw, 24) == 0, "tensor body@16");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 24, 24, "[2,3]int64"),
          "tensor langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewTensor(NULL, 0, raw, 24, "", &buf, &n) != 0,
          "tensor no dims");
}

static void test_ptr_ext(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    /* 指针性只在 langtype：`*` + 被指向类型，head 无 ptr 位。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/varx", 5, "int64",
                                  &buf, &n) == 0, "ptr new");
    CHECK(n == 16 + 5 + 9, "ptr len %llu", (unsigned long long)n);
    kvspaceXvalueHead h;
    memcpy(&h, buf, sizeof h);
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_LENGTH &&
          kvspaceXvalueHeadLen(&h) == 5, "ptr head");
    CHECK(buf[13] == 0 && buf[14] == 0 && buf[15] == 0, "ptr padding zero");
    CHECK(memcmp(buf + KVSPACE_XVALUE_HEAD_LEN, "/varx", 5) == 0, "ptr body");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 5, 5, "*int64"),
          "ptr langtype attr");
    free(buf);

    /* None 指针 = `*T` + 空 body，普通空串用另一类型区分。 */
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 0, "int64", &buf, &n) == 0,
          "none ptr new");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 0, 0, "*int64"),
          "none ptr is *T + empty body");
    free(buf);

    /* 不约束指针：空 langtype → 裸 "*"（前缀不能丢，否则与普通 None 无双射）。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, "", &buf, &n) == 0,
          "unconstrained ptr new");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 2, 2, "*"),
          "unconstrained ptr is bare star");
    free(buf);

    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://b/k", 8, "int64",
                                  &buf, &n) == 0, "ext new");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 8, 8, "@int64"),
          "ext langtype attr");
    free(buf);
    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"s3://", 5, "", &buf, &n) == 0,
          "unconstrained ext new");
    CHECK(value_ok(buf, n, KVSPACE_XVALUE_LAYOUT_LENGTH, 5, 5, "@"),
          "unconstrained ext is bare at");
    free(buf);

    /* 调用方已带前缀 = 两处都以为自己拥有前缀 → 拒绝，绝不产出 "**T"。 */
    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/x", 2, "*int64", &buf, &n) != 0,
          "ptr rejects already-prefixed langtype");
    CHECK(kvspaceXvalueHeadNewExt((const uint8_t *)"/x", 2, "@int64", &buf, &n) != 0,
          "ext rejects already-prefixed langtype");
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 1, "", &buf, &n) != 0, "ptr NULL body");
    CHECK(kvspaceXvalueHeadNewExt(NULL, 1, "", &buf, &n) != 0, "ext NULL body");
    CHECK(kvspaceXvalueHeadNewExt(NULL, 0, "", &buf, &n) != 0, "empty ext");
}

static void test_reserve_code(void) {
    uint8_t *value = NULL;
    uint64_t len = 0;
    /* cap 10 > 有效 7：layout=1，body 区 10 字节零填，reallen=7。 */
    CHECK(kvspaceXvalueHeadReserve(7, 10, "[]u8", &value, &len) == 0,
          "reserve capacity");
    CHECK(len == 44, "reserve len %llu", (unsigned long long)len);
    kvspaceXvalueHead h;
    memcpy(&h, value, sizeof h);
    CHECK(h.layout == KVSPACE_XVALUE_LAYOUT_CAPACITY &&
          kvspaceXvalueHeadLen(&h) == 10, "reserve head");
    CHECK(value_ok(value, len, KVSPACE_XVALUE_LAYOUT_CAPACITY, 10, 7, "[]u8"),
          "reserve langtype + reallen");
    for (uint64_t i = 0; i < 10; i++)
        CHECK(value[KVSPACE_XVALUE_HEAD_LEN + i] == 0, "reserve body zero");
    free(value);

    /* cap == 有效：layout=0，body 区即有效长度。 */
    CHECK(kvspaceXvalueHeadReserve(7, 7, "[]u8", &value, &len) == 0,
          "reserve exact");
    CHECK(value_ok(value, len, KVSPACE_XVALUE_LAYOUT_LENGTH, 7, 7, "[]u8"),
          "reserve exact layout");
    free(value);

    CHECK(kvspaceXvalueHeadReserve(10, 5, "", &value, &len) != 0,
          "reserve body>cap");

    const uint8_t call_body[] = {0, 0, 0, 0, 0, 'f'};
    CHECK(kvspaceXvalueHeadNewCode(call_body, sizeof call_body, sizeof call_body,
                                   "rwir", &value, &len) == 0, "code new");
    CHECK(value_ok(value, len, KVSPACE_XVALUE_LAYOUT_LENGTH, sizeof call_body,
                   sizeof call_body, "rwir"),
          "code langtype attr");
    CHECK(memcmp(value + KVSPACE_XVALUE_HEAD_LEN, call_body,
                 sizeof call_body) == 0, "code body");
    free(value);
    CHECK(kvspaceXvalueHeadNewCode(call_body, sizeof call_body,
                                   sizeof call_body + 4, "rwir", &value,
                                   &len) == 0, "code capacity new");
    CHECK(value_ok(value, len, KVSPACE_XVALUE_LAYOUT_CAPACITY,
                   sizeof call_body + 4, sizeof call_body, "rwir"),
          "code capacity layout");
    free(value);
    CHECK(kvspaceXvalueHeadNewCode(call_body, 5, 3, "", &value, &len) != 0,
          "code cap<len");
}

/* reserved 与 padding 都是 ABI 的一部分：非零即非法；layout 只许 0/1，
   其它值按非法处理。 */
static void test_nonzero_padding_and_layout(void) {
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar((const uint8_t *)"abcd", 4, "int64",
                                     &buf, &n) == 0, "pad build");
    const uint8_t *lt = (const uint8_t *)"sentinel";
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 5 && lt != NULL,
          "pad zero accepted");
    for (int at = 10; at <= 11; at++) {
        buf[at] = 1;
        kvspaceXvalueHead h;
        memcpy(&h, buf, sizeof h);
        lt = (const uint8_t *)"sentinel";
        CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 0 && lt == NULL,
              "nonzero reserved byte %d rejected", at);
        CHECK(!kvspaceXvalueHeadReservedIsZero(&h),
              "helper sees nonzero reserved");
        CHECK(kvspaceXvalueHeadTotalLen(&h) == 0,
              "total len refuses nonzero reserved");
        uint64_t eff = 0;
        CHECK(kvspaceXvalueHeadEffectiveLen(buf, n, &eff) != 0,
              "effective len refuses nonzero reserved");
        buf[at] = 0;
    }
    for (int at = 13; at <= 15; at++) {
        buf[at] = 1;
        kvspaceXvalueHead h;
        memcpy(&h, buf, sizeof h);
        lt = (const uint8_t *)"sentinel";
        CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 0 && lt == NULL,
              "nonzero padding byte %d rejected", at);
        CHECK(!kvspaceXvalueHeadPaddingIsZero(&h), "helper sees nonzero padding");
        CHECK(kvspaceXvalueHeadTotalLen(&h) == 0,
              "total len refuses nonzero padding");
        uint64_t eff = 0;
        CHECK(kvspaceXvalueHeadEffectiveLen(buf, n, &eff) != 0,
              "effective len refuses nonzero padding");
        buf[at] = 0;
    }
    for (int layout = 2; layout <= 255; layout += 253) {
        buf[12] = (uint8_t)layout;
        kvspaceXvalueHead h;
        memcpy(&h, buf, sizeof h);
        CHECK(!kvspaceXvalueHeadLayoutIsValid(&h), "layout %d invalid", layout);
        CHECK(kvspaceXvalueHeadTotalLen(&h) == 0,
              "total len refuses invalid layout");
        uint64_t eff = 0;
        CHECK(kvspaceXvalueHeadEffectiveLen(buf, n, &eff) != 0,
              "effective len refuses invalid layout");
    }
    buf[12] = KVSPACE_XVALUE_LAYOUT_LENGTH;
    free(buf);
}

/* 旧格式（storetype@12 + ptr@13）读取即失败，不做迁移、不做双读。
   注意：旧 storetype=0/ptr=0 的普通值与新 layout=0/padding=0 字节完全重合，
   单看字节不可区分——它的「旧」只在生产端是否写 langtype 前缀，属生产契约。 */
static void test_old_format_rejected(void) {
    /* 旧 Ptr：storetype=1（SLACK）、ptr=1 → 新 padding[0] 非零。 */
    uint8_t old_ptr[16 + 1 + 8] = {0};
    old_ptr[0] = 1;  /* bodylen */
    old_ptr[8] = 8;  /* attrslen: tag1 + "int64" */
    old_ptr[12] = 1; /* 旧 storetype=SLACK */
    old_ptr[13] = 1; /* 旧 ptr=PTR → 新 padding[0] */
    old_ptr[16 + 1] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
    old_ptr[16 + 1 + 1] = 5;
    memcpy(old_ptr + 16 + 1 + 3, "int64", 5);
    const uint8_t *lt = (const uint8_t *)"sentinel";
    CHECK(kvspaceXvalueHeadLangtypeBounded(old_ptr, sizeof old_ptr, &lt) == 0 &&
              lt == NULL,
          "old ptr wire rejected (ptr bit lands in padding)");
    uint64_t eff = UINT64_MAX;
    CHECK(kvspaceXvalueHeadEffectiveLen(old_ptr, sizeof old_ptr, &eff) != 0 &&
              eff == 0,
          "old ptr wire effective len rejected");

    /* 旧 Ext：storetype=3 → 新 layout=3 非法。 */
    uint8_t old_ext[16 + 5] = {0};
    old_ext[0] = 5;
    old_ext[12] = 3;
    old_ext[13] = 2;
    CHECK(kvspaceXvalueHeadEffectiveLen(old_ext, sizeof old_ext, &eff) != 0,
          "old ext wire rejected (storetype 3 is not a layout)");

    /* 旧 VARLEN：storetype=4 → 新 layout=4 非法。 */
    uint8_t old_varlen[16 + 4 + 11] = {0};
    old_varlen[0] = 4;
    old_varlen[8] = 11;
    old_varlen[12] = 4;
    CHECK(kvspaceXvalueHeadEffectiveLen(old_varlen, sizeof old_varlen, &eff) != 0,
          "old varlen wire rejected (storetype 4 is not a layout)");

    /* 旧 SLACK（storetype=1、ptr=0）：新 layout=1 却没有 tag 2 → 拒。 */
    uint8_t old_slack[16 + 2 + 8] = {0};
    old_slack[0] = 2;
    old_slack[8] = 8;
    old_slack[12] = 1;
    old_slack[16 + 2] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
    old_slack[16 + 2 + 1] = 5;
    memcpy(old_slack + 16 + 2 + 3, "int64", 5);
    CHECK(kvspaceXvalueHeadEffectiveLen(old_slack, sizeof old_slack, &eff) != 0,
          "old slack wire rejected (layout=1 without reallen)");

    /* 旧普通标量（storetype=0、ptr=0）：字节与新格式重合，读侧接受。
       它不是「坏 wire」，只是生产端须按新契约写 `*`/`@` 前缀。 */
    uint8_t old_scalar[16 + 8 + 8] = {0};
    old_scalar[0] = 8;
    old_scalar[8] = 8;
    old_scalar[16 + 8] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
    old_scalar[16 + 8 + 1] = 5;
    memcpy(old_scalar + 16 + 8 + 3, "int64", 5);
    CHECK(kvspaceXvalueHeadEffectiveLen(old_scalar, sizeof old_scalar, &eff) == 0 &&
              eff == 8,
          "old scalar bytes are indistinguishable and read as layout=0");
    lt = NULL;
    CHECK(kvspaceXvalueHeadLangtypeBounded(old_scalar, sizeof old_scalar, &lt) == 5,
          "old scalar langtype readable");
}

/* tag 2 的归属排他：layout=0 不许带、layout=1 必须恰有一条且 reallen<=容量。 */
static void test_reallen_rules(void) {
    /* layout=0 + tag2 → 拒。 */
    uint8_t tagged[16 + 11] = {0};
    tagged[8] = 11;
    tagged[16] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
    tagged[17] = 8;
    uint64_t eff = 0;
    CHECK(kvspaceXvalueHeadEffectiveLen(tagged, sizeof tagged, &eff) != 0,
          "tag2 under layout=0 rejected");

    /* layout=1 + 重复 tag2 → 拒。 */
    uint8_t dup[16 + 22] = {0};
    dup[8] = 22;
    dup[12] = KVSPACE_XVALUE_LAYOUT_CAPACITY;
    for (int at = 0; at < 2; at++) {
        uint8_t *a = dup + 16 + 11 * at;
        a[0] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
        a[1] = 8;
    }
    CHECK(kvspaceXvalueHeadEffectiveLen(dup, sizeof dup, &eff) != 0,
          "duplicate tag2 rejected");

    /* layout=1 + tag2 长度不是 8 → 拒。 */
    uint8_t badlen[16 + 8 + 4] = {0};
    badlen[0] = 8;
    badlen[8] = 4;
    badlen[12] = KVSPACE_XVALUE_LAYOUT_CAPACITY;
    badlen[16 + 8] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
    badlen[16 + 8 + 1] = 1;
    CHECK(kvspaceXvalueHeadEffectiveLen(badlen, sizeof badlen, &eff) != 0,
          "tag2 record len != 8 rejected");

    /* layout=1 + reallen > 容量 → 拒。 */
    uint8_t over[16 + 8 + 11] = {0};
    over[0] = 8;
    over[8] = 11;
    over[12] = KVSPACE_XVALUE_LAYOUT_CAPACITY;
    over[16 + 8] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
    over[16 + 8 + 1] = 8;
    over[16 + 8 + 3] = 9;
    CHECK(kvspaceXvalueHeadEffectiveLen(over, sizeof over, &eff) != 0,
          "reallen > capacity rejected");

    /* layout=1 + reallen == 0 合法（容量已分配、当前无内容）。 */
    uint8_t empty[16 + 8 + 11] = {0};
    empty[0] = 8;
    empty[8] = 11;
    empty[12] = KVSPACE_XVALUE_LAYOUT_CAPACITY;
    empty[16 + 8] = KVSPACE_XVALUE_ATTR_TAG_REALLEN;
    empty[16 + 8 + 1] = 8;
    CHECK(kvspaceXvalueHeadEffectiveLen(empty, sizeof empty, &eff) == 0 &&
              eff == 0,
          "reallen 0 legal");
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

    CHECK(kvspaceXvalueHeadNewSlack((const uint8_t *)"xyz", 3, 9, "",
                                    &buf, &n) == 0, "total capacity build");
    memcpy(&h, buf, sizeof h);
    CHECK(kvspaceXvalueHeadTotalLen(&h) == n,
          "total len uses bodylen=capacity for layout=1");
    CHECK(kvspaceXvalueHeadAttrs(buf) == buf + KVSPACE_XVALUE_HEAD_LEN + 9,
          "attrs start after the capacity-sized body");
    free(buf);
}

/* Failure contract and bounded langtype parsing:
   - All constructors return -1 and clear *out = NULL on failure;
   - LangtypeBounded returns the exact non-NUL-terminated slice for valid values
     and 0 with *out = NULL on truncated/malformed inputs without reading out of bounds. */
static void test_bounded_and_reserve_fail(void) {
    uint8_t sentinel = 0x5a;
    uint8_t *buf = &sentinel;
    uint64_t n = 0;
    const uint8_t *lt = &sentinel;

    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(8, 4, "", &buf, &n) == -1 && buf == NULL,
          "Reserve(cap < body_len) rejected and clears *out");
    CHECK(kvspaceXvalueHeadReserve(4, 4, "", NULL, &n) == -1,
          "Reserve(out=NULL) rejected");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadReserve(4, 4, "", &buf, NULL) == -1 && buf == NULL,
          "Reserve(out_len=NULL) rejected and clears *out");

    /* Other constructors also clear *out on failure */
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewScalar(NULL, 3, "", &buf, &n) == -1 && buf == NULL,
          "NewScalar(NULL, 3) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewSlack(NULL, 4, 4, "", &buf, &n) == -1 && buf == NULL,
          "NewSlack(NULL, 4) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewTensor(NULL, 0, &sentinel, 1, "", &buf, &n) == -1 &&
              buf == NULL,
          "NewTensor(NULL dims) clears *out");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewPtr(NULL, 0, "", NULL, &n) == -1,
          "NewPtr(out=NULL) rejected");
    buf = &sentinel;
    CHECK(kvspaceXvalueHeadNewExt(NULL, 0, "", &buf, &n) == -1 && buf == NULL,
          "NewExt(empty) clears *out");

    /* LangtypeBounded valid inputs: scalar / tensor / ptr / empty langtype */
    uint8_t raw8[8] = {1, 0, 0, 0, 0, 0, 0, 0};
    CHECK(kvspaceXvalueHeadNewScalar(raw8, 8, "int64", &buf, &n) == 0,
          "scalar build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 5 && lt != NULL &&
              memcmp(lt, "int64", 5) == 0,
          "bounded scalar langtype");
    free(buf);

    uint64_t dims[2] = {2, 3};
    uint8_t raw24[24] = {0};
    CHECK(kvspaceXvalueHeadNewTensor(dims, 2, raw24, sizeof raw24, "[2,3]float32",
                                     &buf, &n) == 0, "tensor build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 12 && lt != NULL &&
              memcmp(lt, "[2,3]float32", 12) == 0,
          "bounded tensor langtype");
    free(buf);

    CHECK(kvspaceXvalueHeadNewPtr((const uint8_t *)"/dst", 4, "[]char/utf8",
                                  &buf, &n) == 0, "ptr build");
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(buf, n, &lt) == 12 && lt != NULL &&
              memcmp(lt, "*[]char/utf8", 12) == 0,
          "bounded ptr langtype keeps the star");
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
          "bounded wire_len=9 (< 16)");

    /* 截断的 wire：head 说 body+attrs 比实际字节多（head 自己就必须能挡住）。 */
    CHECK(kvspaceXvalueHeadReserve(4, 4, "int32", &buf, &n) == 0,
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

    /* attrslen 放不下一个 record 头（< 3）：无可解析记录 → 无 langtype。 */
    uint8_t bad_alen[16 + 1] = {0};
    bad_alen[8] = 1;
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(bad_alen, sizeof bad_alen, &lt) == 0 &&
              lt == NULL,
          "bounded tolerates trailing bytes shorter than a record header");

    /* Truncated record: attrslen fits in wire_len, but p + 3 + rlen > end */
    uint8_t trunc_rec[16 + 7] = {
        0, 0, 0, 0, 0, 0, 0, 0,             /* head: bodylen=0 */
        7, 0, 0, 0,                         /* attrslen=7 (end = 16+7) */
        0, 0, 0, 0,                         /* layout/padding */
        KVSPACE_XVALUE_ATTR_TAG_LANGTYPE,   /* tag = 1 */
        10, 0,                              /* rlen = 10 -> p+3+10 = 29 > 23 */
        'i', '6'
    };
    lt = &sentinel;
    CHECK(kvspaceXvalueHeadLangtypeBounded(trunc_rec, sizeof trunc_rec, &lt) == 0 &&
              lt == NULL,
          "bounded rejects truncated attr record (p + 3 + rlen > end)");

    /* Trailing record header shorter than 3 bytes (attrslen = 2 leaves 2 bytes) */
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
       record 1 is tag=9 (unknown), record 2 is tag=1 (langtype="int64")
       immediately followed by non-NUL byte 9 (next record tag); verify exact
       rlen=5 slice. */
    uint8_t multi_non_nul[16 + (3 + 2) + (3 + 5) + (3 + 1)] = {
        0, 0, 0, 0, 0, 0, 0, 0,             /* bodylen=0 */
        17, 0, 0, 0,                         /* attrslen = 5 + 8 + 4 = 17 */
        0, 0, 0, 0,
        9, 2, 0, 0xaa, 0xbb,                /* tag=9 (unknown), rlen=2 */
        KVSPACE_XVALUE_ATTR_TAG_LANGTYPE, 5, 0, 'i', 'n', 't', '6', '4', /* tag=1, rlen=5 */
        9, 1, 0, 'Z'                        /* tag=9, rlen=1 -> lt[5] == 9 != '\0' */
    };
    lt = &sentinel;
    uint16_t got_len = kvspaceXvalueHeadLangtypeBounded(multi_non_nul, sizeof multi_non_nul, &lt);
    CHECK(got_len == 5 && lt != NULL && lt[5] == 9 && memcmp(lt, "int64", 5) == 0,
          "bounded returns exact non-NUL-terminated slice after skipping unknown tag");
}

/* attrslen 是 u16：3 字节记录头 + langtype 数据必须 <= UINT16_MAX。 */
static void test_attrslen_limit(void) {
    const size_t max_langtype = UINT16_MAX - 3u;
    const size_t too_long_langtype = max_langtype + 1u;
    char *lt = (char *)malloc(too_long_langtype + 1u);
    CHECK(lt != NULL, "attrslen limit allocation");
    if (!lt)
        return;
    memset(lt, 'a', too_long_langtype);
    lt[max_langtype] = '\0';
    uint8_t *buf = NULL;
    uint64_t n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(NULL, 0, lt, &buf, &n) == 0,
          "attrslen == UINT16_MAX accepted");
    if (buf) {
        kvspaceXvalueHead h;
        memcpy(&h, buf, sizeof h);
        CHECK(kvspaceXvalueHeadAttrsLen(&h) == UINT16_MAX,
              "max attrslen lands in the u16 field");
        free(buf);
    }
    lt[max_langtype] = 'a';
    lt[too_long_langtype] = '\0';
    buf = NULL;
    n = 0;
    CHECK(kvspaceXvalueHeadNewScalar(NULL, 0, lt, &buf, &n) != 0 && buf == NULL,
          "attrslen > UINT16_MAX rejected");
    free(lt);
}

int main(void) {
    test_nonzero_padding_and_layout();
    test_attrslen_limit();
    test_old_format_rejected();
    test_reallen_rules();
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
