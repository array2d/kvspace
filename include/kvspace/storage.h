#ifndef KVSPACE_STORAGE_H
#define KVSPACE_STORAGE_H

/*
 * 存储层 ABI（最小集）：定位、创建/调整、释放、索引、枚举。
 * 不解释值的语义；上层拿到 value_offset 后用「本进程基址 + 偏移」自行读写。
 *
 * value_offset 是 sbo data 区的字节偏移，不是指针；0 合法。
 * parent_key_anchor = (parent_id << 32) | depth；0 = root。
 * 返回 0 或负 errno；不用 0/-1 当偏移哨兵。
 *
 * 注意：kvspaceMkindex 与旧 kvspace.h 同名但签名不同（新 ABI 无 capacity），
 * 迁移期不得同时 include。
 */

#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum kvspace_status {
    KVSPACE_OK = 0,
    KVSPACE_INVALID = -EINVAL,
    KVSPACE_MISSING = -ENOENT,
    KVSPACE_EXISTS = -EEXIST,
    KVSPACE_ANCHOR_INVALID = -ESTALE,
    KVSPACE_CAPACITY = -ENOSPC,
    KVSPACE_NOT_INDEX = -ENOTDIR,
    KVSPACE_HAS_CHILDREN = -ENOTEMPTY,
    KVSPACE_BACKEND = -EIO
} kvspace_status_t;

/* 生命周期（与旧 ABI 同签名）。 */
void *kvspaceConnect(const char *dsn);
void kvspaceClose(void *h);

/* key → 偏移。anchor 0 = root；失效返回 KVSPACE_ANCHOR_INVALID。
 * 跟随目录叠加（extindex）；不跟 Ptr。 */
int kvspaceResolve(void *handle, const char *key, uint64_t parent_key_anchor,
                   uint64_t *out_value_offset, char *err, uint32_t err_cap);

/* 唯一写原语：key 不存在则创建（清零），存在则调整容量。
 * 容量内原地（旧偏移有效）；超容量搬移（旧偏移失效）。 */
int kvspaceRealloc(void *handle, const char *key, uint64_t body_length,
                   uint64_t attr_length, uint64_t *out_value_offset,
                   char *err, uint32_t err_cap);

/* 删除 key 的值并释放 box；空索引连索引节点一起删；非空索引返回 HAS_CHILDREN。 */
int kvspaceFree(void *handle, const char *key, char *err, uint32_t err_cap);

/* 单键复制；new_key 是目标键（调用方指定，可覆盖）。 */
int kvspaceCp(void *handle, const char *old_key, const char *new_key, char *err,
              uint32_t err_cap);

/* 确保目录键存在（写 None 标记）；不复制、不建成员表。 */
int kvspaceMkindex(void *handle, const char *key, char *err, uint32_t err_cap);

/* 目录叠加：key 下的读取回退到 extension_key 的子树；不复制值。
 * 删除叠加用 kvspaceFree(key)。 */
int kvspaceExtindex(void *handle, const char *key, const char *extension_key,
                    char *err, uint32_t err_cap);

/* 递归删除整棵子树。 */
int kvspaceDeltree(void *handle, const char *key, char *err, uint32_t err_cap);

/* 递归复制子树；目标已存在时先删除再复制。 */
int kvspaceCpdir(void *handle, const char *source_key,
                 const char *destination_key, char *err, uint32_t err_cap);

/* 直接子项数；非索引返回 NOT_INDEX，空索引返回 0。 */
int kvspaceListlen(void *handle, const char *key, int64_t *out_count, char *err,
                   uint32_t err_cap);

/* 第 index 个直接子项名（不写 NUL）；越界返回 MISSING，容量不足返回 CAPACITY。 */
int kvspaceListat(void *handle, const char *key, uint64_t index,
                  char *out_name, uint32_t out_capacity, uint32_t *out_length,
                  char *err, uint32_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
