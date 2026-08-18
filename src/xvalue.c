#include "kvspace/xvalue.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* 对齐 kvspace-durable 的 kindexp TLV 编解码。 */

static void wr_u8(uint8_t *dst, uint8_t v)  { dst[0] = v; }
static void wr_u16(uint8_t *dst, uint16_t v) { dst[0]=(uint8_t)v; dst[1]=(uint8_t)(v>>8); }
static void wr_u32(uint8_t *dst, uint32_t v) { for(int i=0;i<4;i++) dst[i]=(uint8_t)(v>>(i*8)); }
static void wr_u64(uint8_t *dst, uint64_t v) { for(int i=0;i<8;i++) dst[i]=(uint8_t)(v>>(i*8)); }
static void wr_i8(uint8_t *dst, int8_t v)   { wr_u8(dst, (uint8_t)v); }
static void wr_i16(uint8_t *dst, int16_t v)  { wr_u16(dst, (uint16_t)v); }
static void wr_i32(uint8_t *dst, int32_t v)  { wr_u32(dst, (uint32_t)v); }
static void wr_i64(uint8_t *dst, int64_t v)  { wr_u64(dst, (uint64_t)v); }

static uint32_t rd_u32(const uint8_t *r) {
    return (uint32_t)r[0] | ((uint32_t)r[1]<<8) | ((uint32_t)r[2]<<16) | ((uint32_t)r[3]<<24);
}

static int32_t header_array_len(int32_t ndim, const int32_t *dims) {
    if (ndim <= 0) return 1;
    int32_t n = 1;
    for (int i = 0; i < ndim; i++) n *= dims[i];
    return n;
}

int32_t xvalue_head_len(const xvalue_head_t *h) {
    return 1 + h->kind_len + 1 + 1 + 4 * h->ndim + 4;
}

static int32_t encode_head(const char *kind, int32_t ref,
                           const int32_t *dims, int32_t ndim,
                           const uint8_t *raw, int32_t raw_len, uint8_t **out) {
    int32_t kl = (int32_t)strlen(kind);
    int32_t total = 1 + kl + 1 + 1 + 4 * ndim + 4 + raw_len;
    uint8_t *buf = (uint8_t *)malloc((size_t)total);
    if (!buf) return -1;
    buf[0] = (uint8_t)kl;
    memcpy(buf + 1, kind, (size_t)kl);
    int32_t o = 1 + kl;
    buf[o++] = (uint8_t)ref;
    buf[o++] = (uint8_t)ndim;
    for (int i = 0; i < ndim; i++) {
        wr_u32(buf + o, (uint32_t)dims[i]);
        o += 4;
    }
    wr_u32(buf + o, (uint32_t)raw_len);
    o += 4;
    if (raw_len > 0 && raw) memcpy(buf + o, raw, (size_t)raw_len);
    *out = buf;
    return total;
}

int32_t xvalue_encode(const char *kind, const uint8_t *raw, int32_t raw_len,
                      const int32_t *dims, int32_t ndim, uint8_t **out) {
    if (!out || !kind) return -1;
    if (ndim < 0) ndim = 0;
    if (ndim > X_MAX_NDIM) return -1;
    if (raw_len < 0) raw_len = 0;
    return encode_head(kind, 0, dims, ndim, raw, raw_len, out);
}

int32_t xvalue_encode_ptr(const char *kind, const uint8_t *raw, int32_t raw_len,
                          const int32_t *dims, int32_t ndim, uint8_t **out) {
    if (!out || !kind) return -1;
    if (ndim < 0) ndim = 0;
    if (ndim > X_MAX_NDIM) return -1;
    if (raw_len < 0) raw_len = 0;
    return encode_head(kind, 1, dims, ndim, raw, raw_len, out);
}

/* 标量/一维便捷编码（内部）：array_len → dims。char/* 恒一维（含空串/单字符）；
 * 其余标量(≤1)=0 维、多元素=1 维。公开的 xvalue_encode 只认 dims/ndim。 */
static int32_t al_to_dims(const char *kind, int32_t array_len, int32_t *dims) {
    if (strncmp(kind, "char/", 5) == 0) { dims[0] = array_len < 0 ? 0 : array_len; return 1; }
    if (array_len > 1) { dims[0] = array_len; return 1; }
    return 0;
}

static int32_t encode_al(const char *kind, const uint8_t *raw, int32_t raw_len,
                         int32_t array_len, uint8_t **out) {
    int32_t dims[1]; int32_t nd = al_to_dims(kind, array_len, dims);
    return xvalue_encode(kind, raw, raw_len, dims, nd, out);
}

static int32_t encode_al_ptr(const char *kind, const uint8_t *raw, int32_t raw_len,
                             int32_t array_len, uint8_t **out) {
    if (array_len <= 0) array_len = 1;
    int32_t dims[1]; int32_t nd = al_to_dims(kind, array_len, dims);
    return xvalue_encode_ptr(kind, raw, raw_len, dims, nd, out);
}

xvalue_head_t xvalue_decode_head(const uint8_t *data, int32_t data_len) {
    xvalue_head_t h = {0};
    if (!data || data_len < 4) return h;
    h.kind_len = (int32_t)data[0];
    int32_t o = 1 + h.kind_len;
    if (data_len < o + 2 + 4) return h;
    h.kind = (const char *)(data + 1);
    h.ref = data[o];
    h.ndim = data[o + 1];
    if (h.ndim > X_MAX_NDIM) return h;
    if (data_len < o + 2 + 4 * h.ndim + 4) return h;
    for (int i = 0; i < h.ndim; i++) h.dims[i] = (int32_t)rd_u32(data + o + 2 + 4 * i);
    int32_t raw_off = o + 2 + 4 * h.ndim;
    h.raw_len = (int32_t)rd_u32(data + raw_off);
    if (data_len < raw_off + 4 + h.raw_len) return h;
    h.raw = data + raw_off + 4;
    h.array_len = header_array_len(h.ndim, h.dims);
    return h;
}

#define DEF_NEW_ARRAY(name, kind, T, elem_sz, wr_fn)                      \
    int32_t xvalue_new_##name(const T *vals, int32_t count, uint8_t **out) { \
        if (!vals || count <= 0) return -1;                                  \
        int32_t raw_len = count * elem_sz;                                   \
        uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);                   \
        if (!raw) return -1;                                                 \
        for (int32_t i = 0; i < count; i++) wr_fn(raw + i * elem_sz, vals[i]); \
        int32_t r = encode_al(kind, raw, raw_len, count, out);               \
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
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);
    if (!raw) return -1;
    for (int32_t i = 0; i < count; i++) {
        union { float f; uint32_t u; } c = {vals[i]};
        wr_u32(raw + i * 4, c.u);
    }
    int32_t r = encode_al(XK_FLOAT32, raw, raw_len, count, out);
    free(raw);
    return r;
}

int32_t xvalue_new_float64(const double *vals, int32_t count, uint8_t **out) {
    if (!vals || count <= 0) return -1;
    int32_t raw_len = count * 8;
    uint8_t *raw = (uint8_t *)malloc((size_t)raw_len);
    if (!raw) return -1;
    for (int32_t i = 0; i < count; i++) {
        union { double f; uint64_t u; } c = {vals[i]};
        wr_u64(raw + i * 8, c.u);
    }
    int32_t r = encode_al(XK_FLOAT64, raw, raw_len, count, out);
    free(raw);
    return r;
}

/* ── char/utf32：UTF-8 → UTF-32 LE ────────────────────────────────── */

static uint32_t utf8_next(const uint8_t *s, int32_t len, int32_t *i) {
    uint32_t cp = s[*i];
    if (cp < 0x80) { (*i)++; return cp; }
    int n;
    if ((cp & 0xE0) == 0xC0)      { n = 1; cp &= 0x1F; }
    else if ((cp & 0xF0) == 0xE0) { n = 2; cp &= 0x0F; }
    else if ((cp & 0xF8) == 0xF0) { n = 3; cp &= 0x07; }
    else { (*i)++; return 0xFFFD; }
    (*i)++;
    for (int j = 0; j < n && *i < len; j++, (*i)++) cp = (cp << 6) | (s[*i] & 0x3F);
    return cp;
}

int32_t xvalue_new_char(const char *s, uint8_t **out) {
    if (!s || !*s) return encode_al(XK_CHAR, NULL, 0, 0, out);
    int32_t slen = (int32_t)strlen(s);
    uint8_t *raw = (uint8_t *)malloc((size_t)slen * 4);
    if (!raw) return -1;
    int32_t n = 0, i = 0;
    while (i < slen) {
        uint32_t cp = utf8_next((const uint8_t *)s, slen, &i);
        wr_u32(raw + n * 4, cp);
        n++;
    }
    int32_t r = encode_al(XK_CHAR, raw, n * 4, n, out);
    free(raw);
    return r;
}

int32_t xvalue_new_char_utf8(const char *s, uint8_t **out) {
    if (!s) s = "";
    int32_t slen = (int32_t)strlen(s);
    return encode_al(XK_CHAR_UTF8, (const uint8_t *)s, slen, slen, out);
}

int32_t xvalue_new_char_ascii(const char *s, uint8_t **out) {
    if (!s) s = "";
    int32_t slen = (int32_t)strlen(s);
    return encode_al(XK_CHAR_ASCII, (const uint8_t *)s, slen, slen, out);
}

int32_t xvalue_at_char(const xvalue_head_t *h, int32_t idx) {
    if (!h || !h->raw || idx < 0 || idx >= h->array_len) return 0;
    return (int32_t)rd_u32(h->raw + idx * 4);
}

/* ── index / ptr / extindex ──────────────────────────────────────── */

int32_t xvalue_new_index(const char **children, int32_t count, uint8_t **out) {
    if (count == 0) return encode_al(XK_INDEX, NULL, 0, 1, out);
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
    int32_t r = encode_al(XK_INDEX, raw, (int32_t)pos, 1, out);
    free(raw);
    return r;
}

int32_t xvalue_new_ptr(const char *kind, const char *target, int32_t array_len, uint8_t **out) {
    if (!target) return -1;
    return encode_al_ptr(kind, (const uint8_t *)target, (int32_t)strlen(target), array_len, out);
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
    int32_t r = encode_al(XK_EXT_INDEX, raw, (int32_t)pos, 1, out);
    free(raw);
    return r;
}
