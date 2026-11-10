#include <kvspace/xvalue_head.h>

#include <stdlib.h>
#include <string.h>

/* Layout: fixed 16-byte header [bodylen:u64@0][attrslen:u16@8][padding@10,16),
   body at offset 16, attr region right after the body with NO length prefix
   (attrslen is in the head — a second prefix would be a second truth).
   Single-key atomic write — no separate attr key. Empty langtype = empty attr.
   bodylen 恒为有效字节数；容量是存储后端的物理分配属性，不进入 wire。
   head 没有 layout / reserved 字段，attr 区没有 reallen tag。 */

/* Failure helper: clear *out per header contract. */
static int fail(uint8_t **out) {
    if (out)
        *out = NULL;
    return -1;
}

/* Single wire builder. Zero-fills the body when body is NULL (Reserve
   placeholder); public entry points validate whether non-zero body_len
   requires non-NULL body.
   langtype_prefix 只由 NewPtr/NewExt 传入 '*' 或 '@'，构造成 langtype 记录的前缀。 */
static int build(const uint8_t *body, uint64_t body_len, char langtype_prefix,
                 const char *langtype, uint8_t **out, uint64_t *out_len) {
    if (!out || !out_len)
        return fail(out);
    *out = NULL;
    const uint64_t ltl = langtype ? (uint64_t)strlen(langtype) : 0;
    const uint64_t record_len = ltl + (langtype_prefix ? 1u : 0u);
    const uint64_t alen = record_len ? 3 + record_len : 0;
    if (alen > UINT16_MAX)
        return -1;
    if (body_len > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN ||
        alen > UINT64_MAX - KVSPACE_XVALUE_HEAD_LEN - body_len)
        return -1;
    const uint64_t n = (uint64_t)KVSPACE_XVALUE_HEAD_LEN + body_len + alen;
    if (n > SIZE_MAX)
        return -1;
    uint8_t *p = (uint8_t *)calloc(1, (size_t)n);
    if (!p)
        return -1;
    kvspaceXvalueHead h = {.padding = {0u, 0u, 0u, 0u, 0u, 0u}};
    kvspaceXvalueHeadSetLen(&h, body_len);
    kvspaceXvalueHeadSetAttrsLen(&h, (uint16_t)alen);
    memcpy(p, &h, sizeof h);
    if (body && body_len)
        memcpy(p + KVSPACE_XVALUE_HEAD_LEN, body, body_len);
    uint8_t *a = p + KVSPACE_XVALUE_HEAD_LEN + body_len;
    if (record_len) {
        a[0] = KVSPACE_XVALUE_ATTR_TAG_LANGTYPE;
        a[1] = (uint8_t)record_len;
        a[2] = (uint8_t)(record_len >> 8);
        uint64_t at = 3;
        if (langtype_prefix)
            a[at++] = (uint8_t)langtype_prefix;
        if (ltl)
            memcpy(a + at, langtype, ltl);
    }
    *out = p;
    *out_len = n;
    return 0;
}

int kvspaceXvalueHeadNewNone(uint8_t **out, uint64_t *out_len) {
    return build(NULL, 0, 0, NULL, out, out_len);
}

int kvspaceXvalueHeadNewScalar(const uint8_t *raw, uint32_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len) {
    if (raw_len && !raw)
        return fail(out);
    return build(raw, raw_len, 0, langtype, out, out_len);
}

int kvspaceXvalueHeadNewSlack(const uint8_t *data, uint64_t data_len,
                              const char *langtype,
                              uint8_t **out, uint64_t *out_len) {
    if (data_len && !data)
        return fail(out);
    return build(data, data_len, 0, langtype, out, out_len);
}

int kvspaceXvalueHeadNewTensor(const uint64_t *dims, uint32_t ndim,
                               const uint8_t *raw, uint64_t raw_len,
                               const char *langtype,
                               uint8_t **out, uint64_t *out_len) {
    /* Shape lives in leading [dims] of langtype; body byte length is raw_len. */
    if (!dims || ndim == 0 || ndim > 64 || (raw_len && !raw))
        return fail(out);
    return build(raw, raw_len, 0, langtype, out, out_len);
}

/* path_len == 0 是合法的 None 指针：`*T` + 空 body。 */
int kvspaceXvalueHeadNewPtr(const uint8_t *path, uint32_t path_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len) {
    if (path_len && !path)
        return fail(out);
    if (langtype && (langtype[0] == '*' || langtype[0] == '@'))
        return fail(out);
    return build(path, path_len, '*', langtype, out, out_len);
}

int kvspaceXvalueHeadNewExt(const uint8_t *locator, uint32_t locator_len,
                            const char *langtype,
                            uint8_t **out, uint64_t *out_len) {
    if (!locator_len || !locator)
        return fail(out);
    if (langtype && (langtype[0] == '*' || langtype[0] == '@'))
        return fail(out);
    return build(locator, locator_len, '@', langtype, out, out_len);
}

int kvspaceXvalueHeadNewCode(const uint8_t *body, uint64_t body_len,
                             const char *langtype,
                             uint8_t **out, uint64_t *out_len) {
    return kvspaceXvalueHeadNewSlack(body, body_len, langtype, out, out_len);
}

int kvspaceXvalueHeadReserve(uint64_t body_len, const char *langtype,
                             uint8_t **out, uint64_t *out_len) {
    /* 预留 = 产 body_len 字节零 body 的占位（后由调用方填充），
       attr 区只含 langtype；容量由存储后端另行保留。 */
    return build(NULL, body_len, 0, langtype, out, out_len);
}

int kvspaceXvalueHeadScan(const uint8_t *wire, uint64_t wire_len,
                          const uint8_t **out_langtype,
                          uint16_t *out_langtype_len) {
    if (out_langtype)
        *out_langtype = NULL;
    if (out_langtype_len)
        *out_langtype_len = 0;
    if (!wire || wire_len < KVSPACE_XVALUE_HEAD_LEN)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    const uint64_t total = kvspaceXvalueHeadTotalLen(h);
    if (total == 0 || total > wire_len)
        return -1;
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + h->bodylen;
    const uint8_t *end = p + kvspaceXvalueHeadAttrsLen(h);
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
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
        } else if (tag == 2u) {
            /* 旧 reallen tag 已删除：出现即拒绝，避免把旧容量 wire 误读成
               有效长度。 */
            return -1;
        }
        p += 3 + rlen;
    }
    if (out_langtype)
        *out_langtype = langtype;
    if (out_langtype_len)
        *out_langtype_len = langtype_len;
    return 0;
}

/* 有效长度 = bodylen；保留入口给调用方统一走 accessor。
   仍完整 Scan 一次，确保 tag 2 / 畸形 attr 不会被静默接受。 */
int kvspaceXvalueHeadEffectiveLen(const uint8_t *wire, uint64_t wire_len,
                                  uint64_t *out) {
    if (out)
        *out = 0;
    if (!out)
        return -1;
    if (kvspaceXvalueHeadScan(wire, wire_len, NULL, NULL) != 0)
        return -1;
    const kvspaceXvalueHead *h = (const kvspaceXvalueHead *)(const void *)wire;
    *out = h->bodylen;
    return 0;
}
