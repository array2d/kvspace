#ifndef KVSPACE_KVSPACE_H
#define KVSPACE_KVSPACE_H

/*
 * 存储层 ABI（最小集）：定位、创建/调整、释放、索引、枚举。
 * 不解释值的语义；上层拿到 value_offset 后用「本进程基址 + 偏移」自行读写。
 *
 * value_offset 是**映射后端**（shm）的 sbo data 区字节偏移，不是指针；0 合法。
 * 无映射后端（durable 的 fs/redis/s3）没有进程内值区，resolve/realloc
 * 返回 KVSPACE_UNSUPPORTED；**本期没有非映射写路径**（durable 暂停；若恢复，
 * 需要新增写入操作或 commit 语义再扩展）。parent_key_anchor 是父节点在 key 区的 int64 字节偏移，
 * 0 = root；内部布局由后端转换，公共 ABI 不暴露。
 * 返回 0 或负 errno；不用 0/-1 当偏移哨兵。
 *
 * **偏移生命周期**：offset 只在「下一次对该 key 的 Realloc/Free 之前」有效。
 * Realloc 容量内原地时旧 offset 仍指向同一盒子；超容量搬移后旧 offset 立即
 * 失效。本 ABI 不提供视图作废/epoch 机制——调用方不得跨写持有 offset，写后
 * 必须重新 Resolve。跨进程并发读者持 offset 与写者搬移同时发生时，结果未定义；
 * shm 后端的并发约束由上层调度保证，不在此 ABI 内。
 *
 * 本头是**唯一公共存储 ABI**。它由原 `storage.h` 正式改名而来；旧一代公共头
 * （`kvspaceGet`/`kvspaceSetValue` 等指针式接口）连同其符号已删除。新代码只
 * 依赖本头声明的 12 个符号。
 */

#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* parent_key_anchor 的根值：从根开始按绝对 key 查找。 */
#define KVSPACE_ANCHOR_ROOT UINT64_C(0)

typedef enum kvspace_status {
    KVSPACE_OK = 0,
    KVSPACE_INVALID = -EINVAL,
    KVSPACE_MISSING = -ENOENT,
    KVSPACE_EXISTS = -EEXIST,
    KVSPACE_ANCHOR_INVALID = -ESTALE,
    KVSPACE_CAPACITY = -ENOSPC,
    KVSPACE_NOT_INDEX = -ENOTDIR,
    KVSPACE_HAS_CHILDREN = -ENOTEMPTY,
    KVSPACE_UNSUPPORTED = -ENOTSUP,
    KVSPACE_BACKEND = -EIO
} kvspace_status_t;

/* 生命周期（与旧 ABI 同签名）。 */

/* 打开一个存储，返回不透明句柄；失败返回 NULL。
 *
 * **只接受 `shm://`, 其余 scheme 一律 NULL。** 判定是字面比较：`dsn` 前 3 个字符必须
 * 是 `shm` 且紧接 `://`，其后整段是文件路径（`shm:///tmp/x.shm` → `/tmp/x.shm`）。
 * `redis://`、`fs://`、`s3://` 都返回 NULL，**不是「连上之后才发现不支持」**。
 *
 * **为什么没有运行期 dispatch**：后端由**链接期**选择——链哪个 libkvspace，就是哪个
 * 后端。这里不 dlopen、不按 DSN 装载任何插件。运行期探测能力再降级的写法，是单库化
 * 要消灭的那条分支：它把「这个部署支持什么」从编译期常量变成运行期惊喜。
 *
 * **失败原因走 `errno`**（与 `fopen`/`malloc` 同一族惯例）：返回 NULL 时必设
 * `errno`——DSN 不合法为 `EINVAL`，文件打不开/映射失败为对应系统 errno。成功返回
 * 非 NULL 时不改动 `errno`，调用方不得用「成功路径上 errno 有值」判断失败。
 *
 * 这条契约写在这里而不是只留在实现里：契约只存在于实现里等于没有契约——换人重写
 * shm 引擎时，这条约束会跟着那 8 行一起消失。只说「返回 NULL」等于让调用方去猜是
 * 格式错、路径不可写、还是布局不匹配——三种故障的修法完全不同。 */
void *kvspaceConnect(const char *dsn);
void kvspaceClose(void *h);

/* 进程内值区基址（地址空间访问器，不计入 9 个语义操作）。
 * 只对映射后端有效：地址 = kvspaceXvalueInProcessAddress(h) + value_offset；
 * shm 的基址在进程生命周期内稳定（预留 VA + MAP_FIXED），跨进程只传 offset，
 * 不传 base。无映射后端（durable）返回 NULL。 */
uint8_t *kvspaceXvalueInProcessAddress(void *handle);

/* key → 偏移。anchor 是**父节点在 key 区的字节偏移**，由后端按自己的 key 数据结构
 * 解释；调用方只持有一个 int64 偏移，不解释内部布局。0 = root（从根按绝对 key 查找）。
 * 偏移失效返回 KVSPACE_ANCHOR_INVALID；不跟 Ptr。 */
int kvspaceResolve(void *handle, const char *key, int64_t parent_key_anchor,
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

/* 建 index 位置：key 必须尾 `·` 或 `/`。只保证位置存在；
 * XValue 的 body（以及 langtype 等）由上层用写路径填写。不建成员表。 */
int kvspaceMkindex(void *handle, const char *key, char *err, uint32_t err_cap);

/* 递归删除整棵子树：key 本身及其全部后代；key 不存在视为成功。 */
int kvspaceDeltree(void *handle, const char *key, char *err, uint32_t err_cap);

/* 递归复制子树；目标已存在时先删除再复制。 */
int kvspaceCpdir(void *handle, const char *source_key,
                 const char *destination_key, char *err, uint32_t err_cap);

/* 直接子项数 N 与最长子项名字节数 M；非索引返回 NOT_INDEX，空索引 N=M=0。
 * M 按 list 返回的同一字节串计（目录项含尾 `/`），供上层一次分配缓冲。 */
int kvspaceListlen(void *handle, const char *key, int64_t *out_count,
                   uint32_t *out_max_name_length, char *err, uint32_t err_cap);

/* 直接子项原始枚举：后端顺序、不排序。名字以 NUL 分隔写入 out_names
 * （目录项带尾 `/`，与旧 ListAt 一致）；out_length 是总字节数。
 * out_names 为 NULL 时只回报长度/个数/generation。
 * 容量不足返回 CAPACITY，且仍写出所需长度与个数，不截断。
 * out_generation 用于上层缓存失效：key 集合变化时前进。 */
int kvspaceList(void *handle, const char *key, char *out_names,
                uint32_t out_capacity, uint32_t *out_length,
                uint32_t *out_count, uint64_t *out_generation, char *err,
                uint32_t err_cap);

#ifdef __cplusplus
}
#endif

#endif
