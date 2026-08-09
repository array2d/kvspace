/*
 * xvalue.h — XValue 类型系统与 TLV 编解码
 *
 * 对齐 kvspace-go: 每个 XValue 天然支持数组，ArrayLen 标识元素数。
 *   NewInt64(42)    → ArrayLen=1, raw=8B
 *   NewInt64(1,2,3) → ArrayLen=3, raw=24B
 *
 * TLV 格式: [1B kind_len][N B kind][4B arraylength LE][4B raw_len LE][M B raw]
 * None 编码为 NULL/len=0。
 */

#ifndef XVAUE_H
#define XVAUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ================================================================
 * Kind 常量
 * ================================================================ */

#define XK_NONE      ""
#define XK_BOOL      "bool"
#define XK_INT8      "int8"
#define XK_INT16     "int16"
#define XK_INT32     "int32"
#define XK_INT64     "int64"
#define XK_UINT8     "uint8"
#define XK_UINT16    "uint16"
#define XK_UINT32    "uint32"
#define XK_UINT64    "uint64"
#define XK_FLOAT32   "float32"
#define XK_FLOAT64   "float64"
#define XK_STRING    "string"
#define XK_BYTES     "bytes"
#define XK_DICT      "dict"
#define XK_INDEX     "index"
#define XK_LINKINDEX "linkindex"
#define XK_EXTINDEX  "extindex"
#define XK_RWIR      "rwir"
#define XK_RWFUNC    "rwfunc"
#define XK_SCOPE     "scope"

/* ================================================================
 * XValueHead — TLV 解码后的头部
 * ================================================================ */

typedef struct {
    const char    *kind;       // 非 NUL-terminated，用 kind_len 比较
    int32_t        kind_len;
    int32_t        array_len;  // None=0, scalar=1, array=N
    int32_t        raw_len;    // raw 段字节数
    const uint8_t *raw;        // 指向 buffer 内部的 raw 段
} xvalue_head_t;

/* ================================================================
 * TLV 编解码
 * ================================================================ */

// 返回完整 TLV 字节数，失败 -1。调用者 free(*out)。
int32_t xvalue_encode(const char *kind, const uint8_t *raw, int32_t raw_len,
                      int32_t array_len, uint8_t **out);
xvalue_head_t xvalue_decode_head(const uint8_t *data, int32_t data_len);

/* ================================================================
 * raw 读取 helpers
 * ================================================================ */

static inline int8_t   xvalue_raw_int8(const uint8_t *r)   { return (int8_t)r[0]; }
static inline int16_t  xvalue_raw_int16(const uint8_t *r)  { return (int16_t)(r[0]|(r[1]<<8)); }
static inline int32_t  xvalue_raw_int32(const uint8_t *r)  { return (int32_t)(r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24)); }
static inline int64_t  xvalue_raw_int64(const uint8_t *r)  { return (int64_t)r[0]|((int64_t)r[1]<<8)|((int64_t)r[2]<<16)|((int64_t)r[3]<<24)|((int64_t)r[4]<<32)|((int64_t)r[5]<<40)|((int64_t)r[6]<<48)|((int64_t)r[7]<<56); }
static inline uint8_t  xvalue_raw_uint8(const uint8_t *r)  { return r[0]; }
static inline uint16_t xvalue_raw_uint16(const uint8_t *r) { return (uint16_t)(r[0]|(r[1]<<8)); }
static inline uint32_t xvalue_raw_uint32(const uint8_t *r) { return (uint32_t)(r[0]|(r[1]<<8)|(r[2]<<16)|(r[3]<<24)); }
static inline uint64_t xvalue_raw_uint64(const uint8_t *r) { return (uint64_t)r[0]|((uint64_t)r[1]<<8)|((uint64_t)r[2]<<16)|((uint64_t)r[3]<<24)|((uint64_t)r[4]<<32)|((uint64_t)r[5]<<40)|((uint64_t)r[6]<<48)|((uint64_t)r[7]<<56); }

static inline float  xvalue_raw_float32(const uint8_t *r) { union{uint32_t u;float f;}v;v.u=xvalue_raw_uint32(r);return v.f;}
static inline double xvalue_raw_float64(const uint8_t *r) { union{uint64_t u;double f;}v;v.u=xvalue_raw_uint64(r);return v.f;}

// At(idx): 从 raw 读取第 idx 个元素
static inline int8_t   xvalue_at_int8(const xvalue_head_t *h, int32_t idx)   { return xvalue_raw_int8(h->raw+idx); }
static inline int16_t  xvalue_at_int16(const xvalue_head_t *h, int32_t idx)  { return xvalue_raw_int16(h->raw+idx*2); }
static inline int32_t  xvalue_at_int32(const xvalue_head_t *h, int32_t idx)  { return xvalue_raw_int32(h->raw+idx*4); }
static inline int64_t  xvalue_at_int64(const xvalue_head_t *h, int32_t idx)  { return xvalue_raw_int64(h->raw+idx*8); }
static inline uint8_t  xvalue_at_uint8(const xvalue_head_t *h, int32_t idx)  { return xvalue_raw_uint8(h->raw+idx); }
static inline uint16_t xvalue_at_uint16(const xvalue_head_t *h, int32_t idx) { return xvalue_raw_uint16(h->raw+idx*2); }
static inline uint32_t xvalue_at_uint32(const xvalue_head_t *h, int32_t idx) { return xvalue_raw_uint32(h->raw+idx*4); }
static inline uint64_t xvalue_at_uint64(const xvalue_head_t *h, int32_t idx) { return xvalue_raw_uint64(h->raw+idx*8); }
static inline float    xvalue_at_float32(const xvalue_head_t *h, int32_t idx) { return xvalue_raw_float32(h->raw+idx*4); }
static inline double   xvalue_at_float64(const xvalue_head_t *h, int32_t idx) { return xvalue_raw_float64(h->raw+idx*8); }
static inline bool     xvalue_at_bool(const xvalue_head_t *h, int32_t idx)    { return h->raw[idx] != 0; }

/* ================================================================
 * 构造函数 — vals=元素数组, count=元素个数(即 ArrayLen)
 *
 * 对齐 Go: NewInt64(42) → count=1; NewInt64(1,2,3) → count=3
 * 返回完整 TLV 字节数，调用者 free(*out)。失败返回 -1。
 * ================================================================ */

int32_t xvalue_new_none(uint8_t **out);

int32_t xvalue_new_bool(const bool *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_int8(const int8_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_int16(const int16_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_int32(const int32_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_int64(const int64_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_uint8(const uint8_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_uint16(const uint16_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_uint32(const uint32_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_uint64(const uint64_t *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_float32(const float *vals, int32_t count, uint8_t **out);
int32_t xvalue_new_float64(const double *vals, int32_t count, uint8_t **out);

int32_t xvalue_new_char(const char *s, uint8_t **out);  // ArrayLen = UTF-8 rune count
int32_t xvalue_at_char(const xvalue_head_t *h, int32_t idx);    // 返回第 idx 个 rune (Unicode codepoint)
int32_t xvalue_new_bytes(const uint8_t *raw, int32_t len, uint8_t **out);
int32_t xvalue_new_index(const char **children, int32_t count, uint8_t **out);
int32_t xvalue_new_linkindex(const char *target, uint8_t **out);
int32_t xvalue_new_extindex(const char *extpath, const char **children, int32_t count, uint8_t **out);

/* ================================================================
 * 便捷：单元素构造 macro
 * ================================================================ */

#define xvalue_new_bool_1(v, out)     xvalue_new_bool(&(bool){v}, 1, out)
#define xvalue_new_int8_1(v, out)     xvalue_new_int8(&(int8_t){v}, 1, out)
#define xvalue_new_int16_1(v, out)    xvalue_new_int16(&(int16_t){v}, 1, out)
#define xvalue_new_int32_1(v, out)    xvalue_new_int32(&(int32_t){v}, 1, out)
#define xvalue_new_int64_1(v, out)    xvalue_new_int64(&(int64_t){v}, 1, out)
#define xvalue_new_uint8_1(v, out)    xvalue_new_uint8(&(uint8_t){v}, 1, out)
#define xvalue_new_uint16_1(v, out)   xvalue_new_uint16(&(uint16_t){v}, 1, out)
#define xvalue_new_uint32_1(v, out)   xvalue_new_uint32(&(uint32_t){v}, 1, out)
#define xvalue_new_uint64_1(v, out)   xvalue_new_uint64(&(uint64_t){v}, 1, out)
#define xvalue_new_float32_1(v, out)  xvalue_new_float32(&(float){v}, 1, out)
#define xvalue_new_float64_1(v, out)  xvalue_new_float64(&(double){v}, 1, out)

#endif
