/*
 * xvalue.h — XValue 类型系统与 TLV 编解码（对齐 kvspace-durable 的 kindexp TLV）。
 *
 * TLV: [1B kind_len][kind][1B ref][1B ndim][ndim×4B dims LE][4B raw_len LE][raw]
 *   ref: 0=内联 1=软链接(Ptr, raw=目标路径) 2=扩展句柄(@)
 *   ndim: 0=标量(单值)，N=N 维数组；ndim 是唯一「是否数组」标志（无独立 arr_flag）
 *   dims: 各维长度（LE u32）
 *   char/* kind 恒为一维序列（ndim=1，含空串/单字符）
 * None 编码为 NULL/len=0。
 */

#ifndef XVAUE_H
#define XVAUE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define XK_NONE       ""
#define XK_BOOL       "bool"
#define XK_INT8       "int8"
#define XK_INT16      "int16"
#define XK_INT32      "int32"
#define XK_INT64      "int64"
#define XK_UINT8      "uint8"
#define XK_UINT16     "uint16"
#define XK_UINT32     "uint32"
#define XK_UINT64     "uint64"
#define XK_FLOAT32    "float32"
#define XK_FLOAT64    "float64"
#define XK_CHAR       "char/utf32"
#define XK_CHAR_UTF8  "char/utf8"
#define XK_CHAR_ASCII "char/ascii"
#define XK_DICT       "dict"
#define XK_INDEX      "index"
#define XK_EXT_INDEX  "extindex"
#define XK_RWIR       "rwir"
#define XK_RWFUNC     "rwfunc"
#define XK_SCOPE      "scope"
#define XK_TIME       "time"
#define XK_DURATION   "duration"

#define X_MAX_NDIM 8

typedef struct {
    const char    *kind;      /* 非 NUL-terminated，用 kind_len */
    int32_t        kind_len;
    int32_t        ref;       /* 0=内联 1=软链接 2=扩展句柄 */
    int32_t        ndim;      /* 0=标量，N=N 维数组（唯一「是否数组」标志） */
    int32_t        dims[X_MAX_NDIM];
    int32_t        array_len; /* 派生：标量=1，定长=∏dims，变长=raw_len/elem_size */
    int32_t        raw_len;
    const uint8_t *raw;
} xvalue_head_t;

/* head 字节数（不含 body） */
int32_t xvalue_head_len(const xvalue_head_t *h);

/* 内联编码（ref=0）。array_len<=0 视为 1；>1 编码为一维连续数组。 */
int32_t xvalue_encode(const char *kind, const uint8_t *raw, int32_t raw_len,
                      int32_t array_len, uint8_t **out);
/* 软链接编码（ref=1），raw 为目标路径。 */
int32_t xvalue_encode_ptr(const char *kind, const uint8_t *raw, int32_t raw_len,
                          int32_t array_len, uint8_t **out);
xvalue_head_t xvalue_decode_head(const uint8_t *data, int32_t data_len);

/* raw 读取 helpers（小端） */
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

/* char/utf32：UTF-8 字符串 → UTF-32 LE 码点（4B×N），array_len=码点数 */
int32_t xvalue_new_char(const char *s, uint8_t **out);
/* char/utf8：UTF-8 字节（1B×N） */
int32_t xvalue_new_char_utf8(const char *s, uint8_t **out);
/* char/ascii：ASCII 字节（1B×N） */
int32_t xvalue_new_char_ascii(const char *s, uint8_t **out);
/* 第 idx 个码点（char/utf32） */
int32_t xvalue_at_char(const xvalue_head_t *h, int32_t idx);

int32_t xvalue_new_index(const char **children, int32_t count, uint8_t **out);
int32_t xvalue_new_ptr(const char *kind, const char *target, int32_t array_len, uint8_t **out);
int32_t xvalue_new_extindex(const char *extpath, const char **children, int32_t count, uint8_t **out);

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
