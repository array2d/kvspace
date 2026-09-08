/*
 * kvspace.h — KVSpace C ABI（权威定义）
 *
 * 这是 kvspace-c（SHM）与 kvspace-durable（redis/fs/s3）两个后端共同实现的
 * 唯一 C ABI。消费者只链接 libkvspace（dispatch 前端），运行期按 DSN scheme 选择后端：
 *   shm://...      → libkvspace-c.so.1
 *   其余（redis/fs/s3）→ libkvspace_durable.so.1
 *
 * 头格式（byte-identical，两边一致，由前端静态实现 codec）：
 *   head = [headlen u16 LE][ref u8][storetype u8][ro u8][vid u32 LE][body_len u32 LE]
 *          [storetype 物理字段（变长，按 storetype）][langtype kindexpr 串（占至 headlen）]
 *   body = [body_len B raw]
 *
 *   三正交轴：
 *     ref       存储位置：0=inline（body=值本体）/1=ptr（body=目标 key）/2=@ext（body=扩展定位符）
 *     storetype 物理布局（codec 唯一分派）：NONE / ATOM / ARRAYND / index / extindex
 *     langtype  语义类型真相：完整 kindexpr 串（含 [dims]、map key·value、struct 原型路径、def 族名），
 *               恒为 head 最后一段，无独立长度字段（长度 = headlen − 当前偏移），不含 ptr/ext 前缀。
 *   物理字段：ARRAYND = ndim u8 + dims[ndim] u32 LE；index/extindex = 成员名矩阵 dims=[len,cap,M]。
 *   body 起始偏移 = headlen。None：storetype=NONE、langtype=""、body 空。
 */

#ifndef KVSPACE_H
#define KVSPACE_H

#include <stdint.h>

#include "kvspace/const.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ref：存储位置维（head 第 2 字节）。只决定 body 语义与是否间接寻址。 */
#define KVSPACE_REF_INLINE 0  /* body = 值本体 raw */
#define KVSPACE_REF_PTR    1  /* body = 目标 key 路径（软链接，单跳同型） */
#define KVSPACE_REF_EXT    2  /* body = 扩展世界定位符（fs 文件 / gpu tensordata） */

/* storetype：物理布局维（head 第 3 字节，codec 唯一分派）。 */
#define KVSPACE_STORETYPE_NONE     0  /* 无物理字段，body 空 */
#define KVSPACE_STORETYPE_ATOM     1  /* 无物理字段，body 定宽 raw */
#define KVSPACE_STORETYPE_ARRAYND  2  /* 物理字段 ndim u8 + dims[ndim] u32 LE，body 稠密等宽数组 */
#define KVSPACE_STORETYPE_INDEX    3  /* 成员名矩阵 dims=[len,cap,M]（静态目录/值容器） */
#define KVSPACE_STORETYPE_EXTINDEX 4  /* 同 index，cap 可增长（运行栈等） */

/* XValue 头（repr C）。三正交轴 ref/storetype/langtype；body 靠 headlen 定位。 */
typedef struct {
    uint16_t headlen;       /* head 总字节数；body 起于偏移 headlen */
    uint8_t  ref;           /* 存储位置：见 KVSPACE_REF_* */
    uint8_t  storetype;     /* 物理布局：见 KVSPACE_STORETYPE_* */
    uint8_t  ro;            /* 1=只读，0=可写 */
    uint32_t vid;           /* vthread id（默认 0） */
    int32_t  body_len;      /* body 字节数 */
    int32_t  ndim;          /* ARRAYND：维数；index/extindex：3（[len,cap,M]）；NONE/ATOM：0 */
    int32_t  dims[8];       /* ARRAYND：各维长度；index/extindex：[len,cap,M] */
    char     langtype[256]; /* 语义类型 kindexpr 串，NUL 终止（含 [dims]、无 ptr/ext 前缀） */
    int32_t  langtype_len;  /* langtype 内容长度（去 padding） */
    int32_t  body_offset;   /* body 在 data 内的起始偏移（= headlen） */
} kvspaceHead_t;

/* ── 生命周期 ─────────────────────────────────────────────────── */
void *kvspaceConnect(const char *dsn);
void  kvspaceClose(void *h);
int   kvspaceDisconnect(void *h, char *err, uint32_t err_cap);

/* ── 单点读写 / 目录 ──────────────────────────────────────────── */

/* 借用读：*out 指向后端常驻空间（shm mmap / durable 借用池），生命周期同该槽，
 * 调用方不得 free。resolve=1 穿透 link。key 不存在/空值 → *out=NULL、*out_len=0、返回 0。 */
int kvspaceGet(void *h, const char *key, int resolve, uint8_t **out, uint32_t *out_len);

/* 指令边界回收读借用池：VM 每条指令执行完调用一次。此后本指令内 Get/GetPart 借出的
 * 指针一律失效。shm 常驻映射侧为 no-op；durable 惰性写不清池，全靠本调用回收。 */
void kvspaceReadReset(void *h);

/* 定位读：借用读 key 值的 [offset, offset+len) 字节，*out 指向后端常驻空间（借用，不得 free，
 * 生命周期至下次 ReadReset）。恒穿透 link。越界/空/不存在 → *out=NULL、*out_len=0、返回 0。 */
int kvspaceGetPart(void *h, const char *key, uint32_t offset, uint32_t len,
                    uint8_t **out, uint32_t *out_len);

/* 定位写：就地写 buf 到 key 值的 [offset, offset+buf_len)（key 须已存在、不改结构/尺寸）。
 * 写即持久。越界/不存在 → 非 0 + err。用于 ndarray 元素/head 的分片就地更新。 */
int kvspaceSetPart(void *h, const char *key, uint32_t offset, const uint8_t *buf,
                    uint32_t buf_len, char *err, uint32_t err_cap);

/* 读 head：只读值前缀并解码三正交轴 head（不取 body），供 xv 系列不借整块即得元数据。
 * 空/不存在 → 返回非 0。 */
int kvspaceGetHead(void *h, const char *key, kvspaceHead_t *out);

/* 就地写：key 必须已存在、kind 不变、body_len 必须等于原 body_len——返回原 box 的 body
 * 偏移指针供调用方直接写。违反前置条件 → 非 0 + err（绝不静默重分配、绝不回落）。写即持久。 */
int kvspaceWriteInPlace(void *h, const char *key, int resolve, uint32_t body_len,
                        uint8_t **body, char *err, uint32_t err_cap);

/* 新位置写：按 (ref, storetype, ro, vid, langtype, body_len) 分配新 box、写好 head，返回 body
 * 偏移指针供直接写。ARRAYND 的 dims 由 codec 从 langtype 串内的 [dims] 解析落入物理字段。
 * 用于新建 key 或 storetype/尺寸变化。写即持久。 */
int kvspaceWriteNewPlace(void *h, const char *key, uint8_t ref, uint8_t storetype,
                         uint8_t ro, uint32_t vid, const char *langtype, uint32_t body_len,
                         uint8_t **body, char *err, uint32_t err_cap);

/* 只返回前缀下子项计数，无缓冲、无需释放。resolve=1 穿透 link。 */
int kvspaceListLen(void *h, const char *prefix, int expand_ext, int resolve, int32_t *out_count);

/* 索引取项：把前缀下第 idx 个直接子项名写进调用方自备缓冲 buf（容量 buf_cap），*out_len
 * 置该名长度（不含 NUL）。库侧零状态、调用方不得 free。idx 越界或缓冲不足 → 返回非 0
 * （缓冲不足时 *out_len 仍为所需长度，不静默截断）。配合 kvspaceListLen 遍历。
 * resolve=1 穿透 link；expand_ext=1 展开 extindex。 */
int kvspaceListAt(void *h, const char *prefix, int expand_ext, int resolve, int32_t idx,
                  uint8_t *buf, uint32_t buf_cap, uint32_t *out_len);

int kvspaceDel(void *h, const char *const *keys, uint32_t nkeys, char *err, uint32_t err_cap);
int kvspaceDelTree(void *h, const char *prefix, char *err, uint32_t err_cap);
int kvspaceCp(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceCpTree(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceCpList(void *h, const char *src, const char *dst, char *err, uint32_t err_cap);
int kvspaceMkindex(void *h, const char *path, uint32_t capacity, char *err, uint32_t err_cap);
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
