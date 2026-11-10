/* frontend.c — KVSpace dispatch 前端（只实现 <kvspace/kvspace.h>）。
 *
 * 前端按 DSN scheme 选择后端 .so，把 handle 包一层 {dl, backend, storage_vt}。
 * 旧 ABI 的 trampoline 与 vtable 已全部删除：本文件不声明、不转发、不兜底任何
 * 旧符号。后端必须是 kvspace.h 的完整实现，否则新 ABI 调用由
 * frontend_storage.c 的探测闸统一返回 KVSPACE_BACKEND。
 *
 * 后端装载名（后缀随平台）：
 *   shm://...           → libkvspace-c.so.1       / macOS: libkvspace-c.dylib
 *   其余（redis/fs/s3） → libkvspace_durable.so.1 / macOS: libkvspace_durable.dylib
 * 先按 SONAME 用 RTLD_NOLOAD 复用宿主已加载的后端（单二进制 memfd 场景），
 * 未命中再 dlopen：按序试本前端 .so 所在目录、其 kvspace 子目录（dladdr，产物
 * 可整体随目录分发），回退固定目录 Linux /usr/lib/kvspace、macOS /usr/local/lib/kvspace。
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_NOLOAD */
#endif
#include "kvspace/kvspace.h"
#include "kvspace/const.h"
#include "frontend_handle.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct kvspace_handle {
    void       *dl;
    int         borrowed; /* NOLOAD 命中的已加载后端：借用，前端不 dlclose */
    void       *backend;
    void      (*close_backend)(void *h);
    void       *storage_vt; /* 新存储 ABI 的符号表缓存（frontend_storage.c） */
    struct kvspace_handle *next;
} kvspace_handle;

static kvspace_handle *H(void *h) { return (kvspace_handle *)h; }

/* ── 给 frontend_storage.c 的不透明访问器 ───────────────────────────── */

void *kvspace_handle_dl(void *h) { return h ? H(h)->dl : NULL; }
void *kvspace_handle_backend(void *h) { return h ? H(h)->backend : NULL; }
void *kvspace_handle_storage_vt(void *h) { return h ? H(h)->storage_vt : NULL; }
int kvspace_handle_set_storage_vt(void *h, void *vt) {
    if (!h)
        return -1;
    if (H(h)->storage_vt)
        return -1;
    H(h)->storage_vt = vt;
    return 0;
}

/* 活跃句柄表：durable 的写侧惰性（body 由调用方在返回后填，落盘延到下一次 op），故漏调
 * Close 就退出会丢最后一笔写。进程正常退出时前端兜底落盘再关闭，兑现 kvspace.h 的退出保证。 */
static kvspace_handle  *live;
static pthread_mutex_t  live_lock = PTHREAD_MUTEX_INITIALIZER;

static void handle_free(kvspace_handle *x) {
    if (x->close_backend && x->backend)
        x->close_backend(x->backend);
    if (!x->borrowed)
        dlclose(x->dl);
    free(x->storage_vt);
    free(x);
}

static void live_unlink(kvspace_handle *x) {
    pthread_mutex_lock(&live_lock);
    for (kvspace_handle **p = &live; *p; p = &(*p)->next)
        if (*p == x) { *p = x->next; break; }
    pthread_mutex_unlock(&live_lock);
}

/* Exit fallback: close any unclosed handles so durable backends flush pending writes.
 *
 * Uses an ELF destructor rather than atexit: atexit handlers run in LIFO order, so if
 * the host registered its own cleanup before the first Connect, an atexit handler here
 * would free handles before the host cleanup runs (causing use-after-free). Destructors
 * run after all atexit handlers (_dl_fini / dyld finalizer), and dlclose is safe there. */
__attribute__((destructor)) static void exit_close(void) {
    for (;;) {
        pthread_mutex_lock(&live_lock);
        kvspace_handle *x = live;
        if (x) live = x->next;
        pthread_mutex_unlock(&live_lock);
        if (!x) return;
        handle_free(x);
    }
}

/* 单二进制能力标记（无导出符号；构建期守卫据此拒绝旧前端）：
 * 本版本前端支持 RTLD_NOLOAD 复用宿主已加载的后端。 */
__attribute__((used)) static const char kvspace_embed_marker[] =
    "kvspace:embed-noload";

/* ── 后端选择 ───────────────────────────────────────────────────────── */

static const char *backend_soname(const char *dsn) {
    int is_shm = dsn && strncmp(dsn, "shm://", 6) == 0;
#if defined(__APPLE__)
    return is_shm ? "libkvspace-c.dylib" : "libkvspace_durable.dylib";
#else
    return is_shm ? "libkvspace-c.so.1" : "libkvspace_durable.so.1";
#endif
}

/* 候选后端路径（按序装入 cand，返回条数）。
 *   1. 本前端 .so 所在目录 + soname（前端与后端同目录：单目录分发）
 *   2. 本前端 .so 所在目录 + kvspace/ + soname（前端 lib/、后端 lib/kvspace/）
 *   3. 本前端 .so 所在目录 + ../kvspace/ + soname（前端装在子目录里时同效）
 *   4. 固定目录 Linux /usr/lib/kvspace、macOS /usr/local/lib/kvspace（系统安装）
 * 四者按序 dlopen，第一条成功的即后端。 */
#define BACKEND_CANDIDATES 4
static int backend_paths(const char *soname, char cand[BACKEND_CANDIDATES][4096]) {
    int n = 0;
    Dl_info info;
    if (dladdr((const void *)backend_paths, &info) && info.dli_fname) {
        const char *slash = strrchr(info.dli_fname, '/');
        if (slash) {
            const int dlen = (int)(slash - info.dli_fname);
            snprintf(cand[n++], 4096, "%.*s/%s", dlen, info.dli_fname, soname);
            snprintf(cand[n++], 4096, "%.*s/kvspace/%s", dlen,
                     info.dli_fname, soname);
            snprintf(cand[n++], 4096, "%.*s/../kvspace/%s", dlen,
                     info.dli_fname, soname);
        }
    }
#if defined(__APPLE__)
    snprintf(cand[n++], 4096, "/usr/local/lib/kvspace/%s", soname);
#else
    snprintf(cand[n++], 4096, "/usr/lib/kvspace/%s", soname);
#endif
    return n;
}

/* ── 常量查询（const.h 的 X 宏表生成查找表） ───────────────────────── */

#define KVSPACE_ENTRY(name) { #name, name },
static const struct { const char *key; const char *val; } kvspace_consts[] = {
    KVSPACE_KV(KVSPACE_ENTRY)
};
#undef KVSPACE_ENTRY

const char *kvspaceConst(const char *name) {
    if (!name) return NULL;
    for (size_t i = 0; i < sizeof kvspace_consts / sizeof kvspace_consts[0]; i++)
        if (strcmp(kvspace_consts[i].key, name) == 0)
            return kvspace_consts[i].val;
    return NULL;
}

/* ── 生命周期 ───────────────────────────────────────────────────────── */

void *kvspaceConnect(const char *dsn) {
    if (!dsn) return NULL;

    /* 单二进制：后端已由宿主 memfd 加载。按 SONAME + RTLD_NOLOAD 复用同一个
     * 已加载对象（返回引用但不再映射）；命中即为借用，前端不 dlclose。 */
    const char *soname = backend_soname(dsn);
    int borrowed = 0;
    void *dl = dlopen(soname, RTLD_NOW | RTLD_NOLOAD);
    if (dl) {
        borrowed = 1;
    } else {
        char cand[BACKEND_CANDIDATES][4096];
        const int ncand = backend_paths(soname, cand);
        for (int i = 0; i < ncand && !dl; i++)
            dl = dlopen(cand[i], RTLD_NOW | RTLD_LOCAL);
        if (!dl) return NULL;
    }

    void *(*connect)(const char *) = dlsym(dl, "kvspaceConnect");
    void (*close_backend)(void *) = dlsym(dl, "kvspaceClose");
    if (!connect || !close_backend) {
        if (!borrowed) dlclose(dl);
        return NULL;
    }
    void *backend = connect(dsn);
    if (!backend) {
        if (!borrowed) dlclose(dl);
        return NULL;
    }

    kvspace_handle *h = malloc(sizeof(*h));
    if (!h) {
        close_backend(backend);
        if (!borrowed) dlclose(dl);
        return NULL;
    }
    h->dl = dl;
    h->borrowed = borrowed; /* malloc 未初始化：漏赋值会让借用句柄被 dlclose */
    h->backend = backend;
    h->close_backend = close_backend;
    h->storage_vt = NULL;

    pthread_mutex_lock(&live_lock);
    h->next = live;
    live = h;
    pthread_mutex_unlock(&live_lock);
    return h;
}

void kvspaceClose(void *h) {
    if (!h) return;
    kvspace_handle *x = H(h);
    live_unlink(x);
    handle_free(x);
}
