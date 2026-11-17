#ifndef KVSPACE_C_XVALUE_INTERNAL_H
#define KVSPACE_C_XVALUE_INTERNAL_H 
#include <kvspace/kvspace_xvalue.h>
#define KVSPACE_XVALUE_REFERENCE_INVALID 0
#define KVSPACE_XVALUE_REFERENCE_INLINE 1
#define KVSPACE_XVALUE_REFERENCE_PTR 2
#define KVSPACE_XVALUE_REFERENCE_EXT 3
#define KVSPACE_C_ATTR_TAG_LANGTYPE 1u
static inline int kvspace_c_scan_attrs(const uint8_t *wire, uint64_t wire_len,
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
    const uint8_t *p = wire + KVSPACE_XVALUE_HEAD_LEN + kvspaceXvalueHeadLen(h);
    const uint8_t *end = p + kvspaceXvalueHeadAttrsLen(h);
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    while (p + 3 <= end) {
        const uint8_t tag = p[0];
        const uint16_t rlen = (uint16_t)((uint16_t)p[1] | ((uint16_t)p[2] << 8));
        if (p + 3 + rlen > end)
            return -1;
        if (tag == KVSPACE_XVALUE_ATTR_TAG_RETIRED_REALLEN)
            return -1;
        if (tag == KVSPACE_C_ATTR_TAG_LANGTYPE && !langtype) {
            langtype = p + 3;
            langtype_len = rlen;
        }
        p += 3 + rlen;
    }
    if (out_langtype)
        *out_langtype = langtype;
    if (out_langtype_len)
        *out_langtype_len = langtype_len;
    return 0;
}
static inline int kvspace_c_reference_of(const uint8_t *wire, uint64_t wire_len) {
    const uint8_t *langtype = NULL;
    uint16_t langtype_len = 0;
    if (kvspace_c_scan_attrs(wire, wire_len, &langtype, &langtype_len) != 0)
        return KVSPACE_XVALUE_REFERENCE_INVALID;
    if (langtype == NULL || langtype_len == 0)
        return KVSPACE_XVALUE_REFERENCE_INLINE;
    if (langtype[0] == '*')
        return KVSPACE_XVALUE_REFERENCE_PTR;
    if (langtype[0] == '@')
        return KVSPACE_XVALUE_REFERENCE_EXT;
    return KVSPACE_XVALUE_REFERENCE_INLINE;
}
#endif
