#include "kvspace/xvalue.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================
 * TLV 编解码
 * ================================================================ */

int32_t xvalue_encode(const char *kind, const uint8_t *raw, int32_t raw_len,
                      int32_t array_len, uint8_t **out) {
    if (!out || !kind) return -1;

    int32_t kind_len = (int32_t)strlen(kind);
    if (kind_len > 255) return -1;
    if (array_len <= 0) array_len = 1;
    if (raw_len < 0) raw_len = 0;

    int32_t total = 1 + kind_len + 4 + 4 + raw_len;
    uint8_t *buf = (uint8_t *)malloc(total);
    if (!buf) return -1;

    buf[0] = (uint8_t)kind_len;
    memcpy(buf + 1, kind, kind_len);

    int32_t off = 1 + kind_len;
    buf[off++] = (uint8_t)(array_len);
    buf[off++] = (uint8_t)(array_len >> 8);
    buf[off++] = (uint8_t)(array_len >> 16);
    buf[off++] = (uint8_t)(array_len >> 24);

    buf[off++] = (uint8_t)(raw_len);
    buf[off++] = (uint8_t)(raw_len >> 8);
    buf[off++] = (uint8_t)(raw_len >> 16);
    buf[off++] = (uint8_t)(raw_len >> 24);

    if (raw_len > 0 && raw) memcpy(buf + off, raw, raw_len);

    *out = buf;
    return total;
}

xvalue_head_t xvalue_decode_head(const uint8_t *data, int32_t data_len) {
    xvalue_head_t h = {NULL, 0, 0, 0, NULL};
    if (!data || data_len < 9) return h;

    h.kind_len = (int32_t)data[0];
    if (data_len < 1 + h.kind_len + 8) return h;
    h.kind = (const char *)(data + 1);

    int32_t off = 1 + h.kind_len;
    h.array_len = (int32_t)(data[off] | (data[off+1] << 8) |
                            (data[off+2] << 16) | (data[off+3] << 24));
    off += 4;
    h.raw_len = (int32_t)(data[off] | (data[off+1] << 8) |
                          (data[off+2] << 16) | (data[off+3] << 24));
    off += 4;
    if (data_len < off + h.raw_len) return h;
    h.raw = data + off;
    return h;
}

/* ================================================================
 * raw 写入 helper — 小端
 * ================================================================ */

static void wr_u8(uint8_t *dst, uint8_t v)  { dst[0] = v; }
static void wr_u16(uint8_t *dst, uint16_t v) { dst[0]=(uint8_t)v; dst[1]=(uint8_t)(v>>8); }
static void wr_u32(uint8_t *dst, uint32_t v) { for(int i=0;i<4;i++) dst[i]=(uint8_t)(v>>(i*8)); }
static void wr_u64(uint8_t *dst, uint64_t v) { for(int i=0;i<8;i++) dst[i]=(uint8_t)(v>>(i*8)); }

static void wr_i8(uint8_t *dst, int8_t v)   { wr_u8(dst, (uint8_t)v); }
static void wr_i16(uint8_t *dst, int16_t v)  { wr_u16(dst, (uint16_t)v); }
static void wr_i32(uint8_t *dst, int32_t v)  { wr_u32(dst, (uint32_t)v); }
static void wr_i64(uint8_t *dst, int64_t v)  { wr_u64(dst, (uint64_t)v); }

/* ================================================================
 * 构造函数
 * ================================================================ */

int32_t xvalue_new_none(uint8_t **out) { *out = NULL; return 0; }

#define DEF_NEW_ARRAY(name, kind, T, elem_sz, wr_fn)                      \
    int32_t xvalue_new_##name(const T *vals, int32_t count, uint8_t **out) { \
        if (!vals || count <= 0) return -1;                                  \
        int32_t raw_len = count * elem_sz;                                   \
        uint8_t *raw = (uint8_t *)malloc(raw_len);                           \
        if (!raw) return -1;                                                 \
        for (int32_t i = 0; i < count; i++) wr_fn(raw + i * elem_sz, vals[i]); \
        int32_t r = xvalue_encode(kind, raw, raw_len, count, out);           \
        free(raw);                                                           \
        return r;                                                            \
    }

DEF_NEW_ARRAY(bool,    XK_BOOL,    bool,    1,  wr_u8)
DEF_NEW_ARRAY(int8,    XK_INT8,    int8_t,  1,  wr_i8)
DEF_NEW_ARRAY(int16,   XK_INT16,   int16_t, 2,  wr_i16)
DEF_NEW_ARRAY(int32,   XK_INT32,   int32_t, 4,  wr_i32)
DEF_NEW_ARRAY(int64,   XK_INT64,   int64_t, 8,  wr_i64)
DEF_NEW_ARRAY(uint8,   XK_UINT8,   uint8_t, 1,  wr_u8)
DEF_NEW_ARRAY(uint16,  XK_UINT16,  uint16_t,2,  wr_u16)
DEF_NEW_ARRAY(uint32,  XK_UINT32,  uint32_t,4,  wr_u32)
DEF_NEW_ARRAY(uint64,  XK_UINT64,  uint64_t,8,  wr_u64)

int32_t xvalue_new_float32(const float *vals, int32_t count, uint8_t **out) {
    if (!vals || count <= 0) return -1;
    int32_t raw_len = count * 4;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return -1;
    for (int32_t i = 0; i < count; i++) {
        union { float f; uint32_t u; } c = {vals[i]};
        wr_u32(raw + i * 4, c.u);
    }
    int32_t r = xvalue_encode(XK_FLOAT32, raw, raw_len, count, out);
    free(raw);
    return r;
}

int32_t xvalue_new_float64(const double *vals, int32_t count, uint8_t **out) {
    if (!vals || count <= 0) return -1;
    int32_t raw_len = count * 8;
    uint8_t *raw = (uint8_t *)malloc(raw_len);
    if (!raw) return -1;
    for (int32_t i = 0; i < count; i++) {
        union { double f; uint64_t u; } c = {vals[i]};
        wr_u64(raw + i * 8, c.u);
    }
    int32_t r = xvalue_encode(XK_FLOAT64, raw, raw_len, count, out);
    free(raw);
    return r;
}

/* ================================================================
 * Char — UTF-8 字符串，ArrayLen = rune 数
 * ================================================================ */

static int32_t utf8_rune_count(const uint8_t *s, int32_t len) {
    int32_t n = 0;
    for (int32_t i = 0; i < len; i++)
        if ((s[i] & 0xC0) != 0x80) n++;
    return n;
}

static int32_t utf8_decode_rune(const uint8_t *s, int32_t *out_len) {
    if ((s[0] & 0x80) == 0)      { *out_len = 1; return s[0]; }
    if ((s[0] & 0xE0) == 0xC0)   { *out_len = 2; return ((s[0]&0x1F)<<6)  | (s[1]&0x3F); }
    if ((s[0] & 0xF0) == 0xE0)   { *out_len = 3; return ((s[0]&0x0F)<<12) | ((s[1]&0x3F)<<6) | (s[2]&0x3F); }
    *out_len = 4;
    return ((s[0]&0x07)<<18) | ((s[1]&0x3F)<<12) | ((s[2]&0x3F)<<6) | (s[3]&0x3F);
}

int32_t xvalue_new_char(const char *s, uint8_t **out) {
    if (!s) return xvalue_encode(XK_STRING, NULL, 0, 0, out);
    int32_t slen = (int32_t)strlen(s);
    int32_t runes = utf8_rune_count((const uint8_t *)s, slen);
    return xvalue_encode(XK_STRING, (const uint8_t *)s, slen, runes, out);
}

int32_t xvalue_at_char(const xvalue_head_t *h, int32_t idx) {
    if (!h || !h->raw || idx < 0 || idx >= h->array_len) return 0;
    int32_t pos = 0;
    for (int32_t i = 0; i < idx; i++) {
        int32_t sz;
        utf8_decode_rune(h->raw + pos, &sz);
        pos += sz;
    }
    int32_t sz;
    return utf8_decode_rune(h->raw + pos, &sz);
}

/* ================================================================
 * Bytes（array_len=1）
 * ================================================================ */

int32_t xvalue_new_bytes(const uint8_t *raw, int32_t len, uint8_t **out) {
    return xvalue_encode(XK_BYTES, raw, len, 1, out);
}

/* ================================================================
 * Index / LinkIndex / ExtIndex
 * ================================================================ */

int32_t xvalue_new_index(const char **children, int32_t count, uint8_t **out) {
    if (count == 0) return xvalue_encode(XK_INDEX, NULL, 0, 1, out);

    size_t total = 0;
    for (int i = 0; i < count; i++) total += (children[i] ? strlen(children[i]) : 0);
    total += (size_t)(count - 1);
    if (total > (size_t)INT32_MAX) return -1;

    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) return -1;

    size_t pos = 0;
    for (int i = 0; i < count; i++) {
        if (!children[i]) continue;
        size_t len = strlen(children[i]);
        memcpy(raw + pos, children[i], len);
        pos += len;
        if (i < count - 1) raw[pos++] = '\n';
    }

    int32_t r = xvalue_encode(XK_INDEX, raw, (int32_t)pos, 1, out);
    free(raw);
    return r;
}

int32_t xvalue_new_linkindex(const char *target, uint8_t **out) {
    if (!target) return -1;
    return xvalue_encode(XK_LINKINDEX, (const uint8_t *)target, (int32_t)strlen(target), 1, out);
}

#define EXT_PREFIX "…"

int32_t xvalue_new_extindex(const char *extpath, const char **children, int32_t count, uint8_t **out) {
    if (!extpath) return -1;

    size_t plen = strlen(EXT_PREFIX) + strlen(extpath);
    size_t total = plen;
    for (int i = 0; i < count; i++) total += (children[i] ? strlen(children[i]) : 0);
    if (count > 0) total += (size_t)count;
    if (total > (size_t)INT32_MAX) return -1;

    uint8_t *raw = (uint8_t *)malloc(total);
    if (!raw) return -1;

    size_t pos = 0;
    memcpy(raw + pos, EXT_PREFIX, strlen(EXT_PREFIX)); pos += strlen(EXT_PREFIX);
    memcpy(raw + pos, extpath, strlen(extpath)); pos += strlen(extpath);

    for (int i = 0; i < count; i++) {
        raw[pos++] = '\n';
        if (!children[i]) continue;
        size_t len = strlen(children[i]);
        memcpy(raw + pos, children[i], len);
        pos += len;
    }

    int32_t r = xvalue_encode(XK_EXTINDEX, raw, (int32_t)pos, 1, out);
    free(raw);
    return r;
}
