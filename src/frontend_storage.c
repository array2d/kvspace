/* frontend_storage.c — 新存储层 ABI（storage.h）的 dispatch 前端。
 *
 * **独立 TU**：即便旧 `<kvspace/kvspace.h>` 已从发布物删除，旧符号仍在 .so 里
 * 共存到 P4/P5，且 `frontend.c` 要自己私有声明它们。新 ABI 的 kvspaceMkindex
 * 与旧 ABI 同名**但签名不同**（旧版带 capacity），kvspaceListlen/kvspaceListLen、
 * kvspaceDeltree/kvspaceDelTree 只差大小写——同 TU 必然重复声明。两份 ABI 混在
 * 一个编译单元里正是二义性的温床，所以分开编译，谁也不需要知道对方存在。
 *
 * 与 frontend.c 共用同一个后端对象：新符号是**同一个 dl** 上的可选导出，不是
 * 第二套连接。handle 布局只有 frontend.c 一个定义点，这里经 frontend_handle.h
 * 的不透明访问器取 dl / backend，看不见也碰不到旧 ABI 的函数指针表。
 * 这里只做两件事：
 *   1. 取 dl / backend；
 *   2. dlsym 新符号并转发；缺失即 KVSPACE_BACKEND，绝不假装成功。
 *
 * 旧 ABI 的符号与行为一行不动：新符号全缺时，旧调用方完全无感。 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "kvspace/kvspace.h"
#include "frontend_handle.h"

#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* ── 新符号的 vt：九个独立函数指针，缺失即为 NULL ─────────────────── */

typedef struct {
    uint8_t *(*in_process_address)(void *h);
    int (*resolve)(void *h, const char *key, int64_t parent_key_anchor,
                   uint64_t *out_value_offset, char *err, uint32_t err_cap);
    int (*realloc)(void *h, const char *key, uint64_t body_length,
                   uint64_t attr_length, uint64_t *out_value_offset,
                   char *err, uint32_t err_cap);
    int (*free_key)(void *h, const char *key, char *err, uint32_t err_cap);
    int (*cp)(void *h, const char *old_key, const char *new_key, char *err,
              uint32_t err_cap);
    int (*mkindex)(void *h, const char *key, char *err, uint32_t err_cap);
    int (*deltree)(void *h, const char *key, char *err, uint32_t err_cap);
    int (*cpdir)(void *h, const char *source_key, const char *destination_key,
                 char *err, uint32_t err_cap);
    int (*listlen)(void *h, const char *key, int64_t *out_count,
                   uint32_t *out_max_name_length, char *err, uint32_t err_cap);
    int (*list)(void *h, const char *key, char *out_names, uint32_t out_capacity,
                uint32_t *out_length, uint32_t *out_count,
                uint64_t *out_generation, char *err, uint32_t err_cap);
} kvspace_storage_vt;

/* dlsym 一次、按 handle 缓存：后端符号在 dlopen 后不会变，每次调用都查表是白烧。
 *
 * ── 新 ABI 探测闸（唯一能防签名错位的手段）─────────────────────────
 *
 * `kvspaceMkindex` 是**新旧共名**符号，签名却不同（旧五参带 capacity，新四参
 * 没有）。动态链接不做签名检查：dlsym 拿到旧五参的函数指针，按新四参调用，
 * 编译期、链接期都安静，只在运行期以参数错位的形式崩掉。所以**绝不能拿它当
 * 「新 ABI 存在」的判据**——它在旧后端上当然存在。
 *
 * 判据只能用**新 ABI 独有**的符号，而且必须是**合取**，不是析取。
 *
 * 为什么不能用「任一命中」（这条是踩出来的）：shm 后端曾出现**半迁移**形态——
 * 已导出 `kvspaceResolve`，但 `kvspaceMkindex` 还是旧五参。析取判据见前者就
 * 放行，随后按新四参调用旧指针，参数错位崩溃。单个独有符号不能证明整批符号
 * 同一代。
 *
 * 所以要求下面四个**同时**存在才放行：
 *   kvspaceResolve / kvspaceXvalueInProcessAddress /
 *   kvspaceListlen（小写 l，与旧 kvspaceListLen 不同名）/ kvspaceMkindex
 *
 * 其中 `kvspaceMkindex` 是共名的，dlsym 看不出签名；它在这里的作用不是「证明
 * 自己」，而是**被其余三个锚定**：三个新独有符号都在，说明后端是新一代码，
 * 那么同批导出的 kvspaceMkindex 就应当是四参。这是间接约束，也是动态链接下
 * 能拿到的最强保证——**不为验签名新增任何导出符号**。
 *
 * 未命中时：**整张 vt 作废**（不保留任何函数指针），所有新 ABI 调用一律走
 * missing() 返回 KVSPACE_BACKEND 并写 err。绝不「能查到几个就调几个」——
 * 部分可用比全不可用更危险，调用方会以为自己在用新 ABI。 */
static kvspace_storage_vt *storage_vt_of(void *h) {
    kvspace_storage_vt *cached = kvspace_handle_storage_vt(h);
    if (cached)
        return cached;
    void *dl = kvspace_handle_dl(h);
    if (!dl)
        return NULL;

    /* 合取探闸：四个符号全在才认为后端已完整迁移。
     * kvspaceMkindex 与其余三个同批出现，才敢按新四参调用它。 */
    const int new_abi_present =
        dlsym(dl, "kvspaceResolve") != NULL &&
        dlsym(dl, "kvspaceXvalueInProcessAddress") != NULL &&
        dlsym(dl, "kvspaceListlen") != NULL &&
        dlsym(dl, "kvspaceMkindex") != NULL;

    kvspace_storage_vt *vt = calloc(1, sizeof(*vt));
    if (!vt)
        return NULL;
    if (new_abi_present) {
#define RESOLVE(field, name) *(void **)&vt->field = dlsym(dl, name)
        RESOLVE(in_process_address, "kvspaceXvalueInProcessAddress");
        RESOLVE(resolve, "kvspaceResolve");
        RESOLVE(realloc, "kvspaceRealloc");
        RESOLVE(free_key, "kvspaceFree");
        RESOLVE(cp, "kvspaceCp");
        /* 共名符号：闸已经证明后端是新的，这里才是安全的新四参。 */
        RESOLVE(mkindex, "kvspaceMkindex");
        RESOLVE(deltree, "kvspaceDeltree");
        RESOLVE(cpdir, "kvspaceCpdir");
        RESOLVE(listlen, "kvspaceListlen");
        RESOLVE(list, "kvspaceList");
#undef RESOLVE
    }
    /* 闸未通过 → vt 保持全零。缓存它（零表）而不是缓存 NULL：否则每次调用都
     * 重新 dlsym 一遍，等于把闸放在热路径上。 */
    if (kvspace_handle_set_storage_vt(h, vt) != 0) {
        /* 注意：不能写 `free(vt)` 之外的花样，也不能把 vt 的字段名与 libc 冲突。
         * 这里曾把结构体字段命名为 `free`，于是 `vt->free` 遮蔽了 libc free，
         * 一次误写就变成「调用后端的 close 去释放一块 malloc 内存」——静默破坏
         * 堆，直到某次调用才炸在别处。字段已改名 `free_key`，见 vt 定义。 */
        free(vt);
        return kvspace_handle_storage_vt(h);
    }
    return vt;
}

/* 缺符号的唯一出口：写 err 并返回 KVSPACE_BACKEND。绝不返回 0（那会让调用方
 * 拿着未初始化的 value_offset 继续跑），也绝不退化成「当作空集合」。
 *
 * err 文案要能区分两件事，否则排障时会找错方向：
 *   - 后端没完整迁移到新 ABI（四个探闸符号没凑齐）；
 *   - 后端是新 ABI，但恰好少导出了这一个符号。
 * 前者要去升级后端，后者是后端打包遗漏——完全不同的处置。 */
static int missing(const kvspace_storage_vt *vt, char *err, uint32_t err_cap,
                   const char *name) {
    /* vt 是零表 = 闸未通过 = 后端没完整迁移。用 resolve 代表整表：闸一旦通过，
     * resolve 一定是 dlsym 过的（哪怕后端真缺它，那也是「迁移了但漏符号」）。 */
    const int migrated = vt && vt->resolve != NULL;
    if (err && err_cap) {
        if (migrated)
            snprintf(err, err_cap, "kvspace: %s not provided by this backend",
                     name);
        else
            snprintf(err, err_cap,
                     "kvspace: backend has no complete new storage ABI "
                     "(need kvspaceResolve + kvspaceXvalueInProcessAddress + "
                     "kvspaceListlen + kvspaceMkindex); "
                     "%s unavailable",
                     name);
    }
    return KVSPACE_BACKEND;
}

/* 取 vt 与后端句柄；handle 为空或 vt 分配失败同样走 KVSPACE_BACKEND。
 * 拿到 NULL 的调用方一律走 missing() 出口——不存在「查不到就当成功」的分支。 */
static kvspace_storage_vt *vt_or_missing(void *h, void **out_backend) {
    if (!h)
        return NULL;
    kvspace_storage_vt *vt = storage_vt_of(h);
    if (!vt)
        return NULL;
    *out_backend = kvspace_handle_backend(h);
    return vt;
}

/* 地址空间访问器：返回基址或 NULL。
 *
 * 与另外九个不同，它**没有 err 通道**——返回 NULL 一种取值同时表示「后端不是
 * 映射型」和「后端没实现这个符号」，调用方本来就只能二选一地回退（用旧 ABI 的
 * kvspaceGet/SetValue），所以不需要区分。**绝不**返回非 NULL 的假基址：那会让
 * 调用方拿 base+offset 去解一段不属于它的地址。 */
uint8_t *kvspaceXvalueInProcessAddress(void *h) {
    if (!h)
        return NULL;
    kvspace_storage_vt *vt = storage_vt_of(h);
    if (!vt || !vt->in_process_address)
        return NULL;
    return vt->in_process_address(kvspace_handle_backend(h));
}

int kvspaceResolve(void *h, const char *key, int64_t parent_key_anchor,
                   uint64_t *out_value_offset, char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->resolve)
        return missing(vt, err, err_cap, "kvspaceResolve");
    return vt->resolve(backend, key, parent_key_anchor, out_value_offset, err,
                       err_cap);
}

int kvspaceRealloc(void *h, const char *key, uint64_t body_length,
                   uint64_t attr_length, uint64_t *out_value_offset,
                   char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->realloc)
        return missing(vt, err, err_cap, "kvspaceRealloc");
    return vt->realloc(backend, key, body_length, attr_length, out_value_offset,
                       err, err_cap);
}

int kvspaceFree(void *h, const char *key, char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->free_key)
        return missing(vt, err, err_cap, "kvspaceFree");
    return vt->free_key(backend, key, err, err_cap);
}

int kvspaceCp(void *h, const char *old_key, const char *new_key, char *err,
              uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->cp)
        return missing(vt, err, err_cap, "kvspaceCp");
    return vt->cp(backend, old_key, new_key, err, err_cap);
}

int kvspaceMkindex(void *h, const char *key, char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->mkindex)
        return missing(vt, err, err_cap, "kvspaceMkindex");
    return vt->mkindex(backend, key, err, err_cap);
}

int kvspaceDeltree(void *h, const char *key, char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->deltree)
        return missing(vt, err, err_cap, "kvspaceDeltree");
    return vt->deltree(backend, key, err, err_cap);
}

int kvspaceCpdir(void *h, const char *source_key, const char *destination_key,
                 char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->cpdir)
        return missing(vt, err, err_cap, "kvspaceCpdir");
    return vt->cpdir(backend, source_key, destination_key, err, err_cap);
}

int kvspaceListlen(void *h, const char *key, int64_t *out_count,
                   uint32_t *out_max_name_length, char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->listlen)
        return missing(vt, err, err_cap, "kvspaceListlen");
    return vt->listlen(backend, key, out_count, out_max_name_length, err,
                       err_cap);
}

int kvspaceList(void *h, const char *key, char *out_names, uint32_t out_capacity,
                uint32_t *out_length, uint32_t *out_count,
                uint64_t *out_generation, char *err, uint32_t err_cap) {
    void *backend = NULL;
    kvspace_storage_vt *vt = vt_or_missing(h, &backend);
    if (!vt || !vt->list)
        return missing(vt, err, err_cap, "kvspaceList");
    return vt->list(backend, key, out_names, out_capacity, out_length, out_count,
                    out_generation, err, err_cap);
}
