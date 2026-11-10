/* frontend_handle.h — frontend.c 与 frontend_storage.c 之间的**内部**契约。
 *
 * 为什么不把 `kvspace_handle` 的结构体定义搬到这里给两个 TU 共用：那会把旧 ABI 的
 * `kvspace_vt` 拖进新 ABI 的编译单元，等于让两份「同名不同义」的头在一个 TU 里
 * 间接共存——正是 src/frontend_storage.c 开头要避免的那种二义性。
 *
 * 所以这里只给**不透明访问器**：新 ABI 侧能拿 dl 与 backend，却看不见 handle 的
 * 布局，也不可能顺手摸到旧 vt 的字段。布局仍然只有 frontend.c 一个定义点。
 *
 * 这些符号**不导出**（hidden）：它们不是 ABI，只是同一张 .so 内部两个 TU 的接线，
 * 外部谁都不该 dlsym 到它们。 */

#ifndef KVSPACE_FRONTEND_HANDLE_H
#define KVSPACE_FRONTEND_HANDLE_H

#ifdef __cplusplus
extern "C" {
#endif

#define KVSPACE_INTERNAL __attribute__((visibility("hidden")))

/* 后端已加载的 dl 句柄；h 为 NULL 时返回 NULL。新 ABI 的 dlsym 就打在它上面。 */
KVSPACE_INTERNAL void *kvspace_handle_dl(void *h);

/* 后端自己的连接对象；h 为 NULL 时返回 NULL。 */
KVSPACE_INTERNAL void *kvspace_handle_backend(void *h);

/* 新 ABI 的符号表缓存（kvspace_storage_vt *），未建立时为 NULL。 */
KVSPACE_INTERNAL void *kvspace_handle_storage_vt(void *h);

/* 写入缓存。只允许写一次（第二次写返回 -1 并保留原值）——两个不同的 vt
 * 说明有人在两处各建了一份符号表，那是分工问题，不是可以静默覆盖的小事。 */
KVSPACE_INTERNAL int kvspace_handle_set_storage_vt(void *h, void *vt);

#ifdef __cplusplus
}
#endif

#endif
