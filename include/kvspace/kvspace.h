/*
 * kvspace.h — KVSpace C ABI（权威定义）
 *
 * 这是 kvspace-c（SHM）与 kvspace-durable（redis/fs/s3）两个后端共同实现的
 * 唯一 C ABI。消费者只链接 libkvspace（dispatch 前端），运行期按 DSN scheme 选择后端：
 *   shm://...      → libkvspace-c.so.1
 *   其余（redis/fs/s3）→ libkvspace_durable.so.1
 *
 * 头格式（byte-identical，两边一致，由前端静态实现 codec）：
 *   [1B kindexprlen][kindexpr 含 0x00 padding][1B ro][4B vid LE][4B raw_len LE][raw]
 *   kindexpr 串首字节 * =指针(Ptr) / @ =扩展句柄 / 无 =内联，其后 [d0,d1]kind 承载 ndim+dims。
 */

#ifndef KVSPACE_H
#define KVSPACE_H

#include <stdint.h>

#include "kvspace/const.h"

#ifdef __cplusplus
extern "C" {
#endif

/* XValue 头（repr C）。kindexpr 为唯一类型真相，body 靠 offset/len 定位。 */
typedef struct {
    uint8_t  kindexpr[256]; /* NUL 终止（含 ref 前缀与 [dims]，去 padding） */
    uint8_t  ro;            /* 1=只读，0=可写 */
    uint32_t vid;           /* vthread id（默认 0） */
    int32_t  body_len;      /* body 字节数 */
    int32_t  body_offset;   /* body 在 data 内的起始偏移（= head 长度） */
} kvspaceHead_t;

/* ── 生命周期 ─────────────────────────────────────────────────── */
void *kvspaceConnect(const char *dsn);
void  kvspaceClose(void *h);
int   kvspaceDisconnect(void *h, char *err, uint32_t err_cap);

/* ── 单点读写 / 目录 ──────────────────────────────────────────── */

/* 借用读：*out 指向后端常驻空间（shm mmap / durable 常驻映射），生命周期同该槽，
 * 调用方不得 free。resolve=1 穿透 link。key 不存在/空值 → *out=NULL、*out_len=0、返回 0。 */
int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len);

/* 就地写：key 必须已存在、kind 不变、body_len 必须等于原 body_len——返回原 box 的 body
 * 偏移指针供调用方直接写。违反前置条件 → 非 0 + err（绝不静默重分配、绝不回落）。写即持久。 */
int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap);

/* 新位置写：按 (kindexpr, body_len) 分配新 box、写好 head，返回 body 偏移指针供直接写。
 * 用于新建 key 或 kind/尺寸变化。写即持久。 */
int kvspaceWriteNewPlace(void *h, const char *key, const char *kindexpr, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap);

/* 只返回前缀下子项计数，无缓冲、无需释放。resolve=1 穿透 link。 */
int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count);

/* 借用索引取项：返回前缀下第 idx 个直接子项名，*out 指向后端常驻/回收缓冲，生命周期至下次
 * 同线程 ListAt，调用方不得 free。idx 越界 → *out=NULL、*out_len=0、返回非 0。配合
 * kvspaceListLen 遍历（listlen 定计数，逐 idx 取名），不再一次性返回整段名单缓冲。
 * resolve=1 穿透 link；expand_ext=1 展开 extindex。 */
int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                  uint8_t **out, uint32_t *out_len);

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap);
int kvspaceCp(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceCpTree(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceMkindex(void *h, const char *path, char *err, uint32_t err_cap);
int kvspaceMkindexExt(void *h, const char *path, const char *ext_path, char *err, uint32_t err_cap);
int kvspaceRmindexExt(void *h, const char *path, char *err, uint32_t err_cap);
int kvspaceClear(void *h, char *err, uint32_t err_cap);
/* 借用：*out 指向后端常驻空间，调用方不得 free。 */
int kvspaceWatch(void *h, const char *key, const uint8_t *target, uint32_t target_len,
                 uint64_t tick_ns, uint8_t **out, uint32_t *out_len);

/* ── codec（无 handle，由前端静态实现，byte-identical） ─────────── */
int kvspaceTlvEncode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                     const int32_t *dims, int32_t ndim, uint8_t **out, uint32_t *out_len);
int kvspaceTlvEncodeMode(const char *kind, const uint8_t *raw, uint32_t raw_len,
                         const int32_t *dims, int32_t ndim, int32_t ref, uint8_t ro, uint32_t vid,
                         uint8_t **out, uint32_t *out_len);
int kvspaceDecodeHead(const uint8_t *data, uint32_t data_len, kvspaceHead_t *out);

int kvspaceNewPtr(const char *target_kindexpr, const char *target,
                  uint8_t **out, uint32_t *out_len);
int kvspaceNewChar(const uint8_t *bytes, uint32_t len, uint8_t **out, uint32_t *out_len);
int kvspaceNewBool(uint8_t v, uint8_t **out, uint32_t *out_len);
int kvspaceNewInt64(int64_t v, uint8_t **out, uint32_t *out_len);
int kvspaceNewFloat64(double v, uint8_t **out, uint32_t *out_len);

#ifdef __cplusplus
}
#endif

#endif
