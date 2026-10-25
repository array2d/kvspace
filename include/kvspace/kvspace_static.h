/* kvspace_static.h — 静态链接（单二进制）内部 API，不属于公共 ABI。
 *
 * 下游没有 .so 可 dlopen 时，后端在 constructor 里把 {scheme, connect, 符号表}
 * 注册给 dispatch 前端；kvspaceConnect 先查表，命中就不碰 dlopen。
 * 符号名与 dlsym 时用的名字一致（公共 ABI 名），值指向后端静态构建里的私有
 * 符号（kvspaceC* / kvspaceD*），因此静态链接不会与前端的定义重复。
 * 共享库构建里本文件的符号均为 hidden。
 */
#ifndef KVSPACE_STATIC_H
#define KVSPACE_STATIC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name; /* 公共 ABI 名，如 "kvspaceGet"（与 dlsym 相同） */
    void *fn;
} kvspace_static_sym_t;

#if defined(KVSPACE_STATIC_BUILD)
#define KVSPACE_STATIC_API
#elif defined(__GNUC__)
#define KVSPACE_STATIC_API __attribute__((visibility("hidden")))
#else
#define KVSPACE_STATIC_API
#endif

/* scheme 精确匹配 DSN 的 "xxx://" 前缀；"*" 作兜底（durable 后端）。
 * 必须在任何 kvspaceConnect 之前调用（constructor 恰好满足）。 */
KVSPACE_STATIC_API void kvspaceStaticRegisterBackend(
    const char *scheme, void *(*connect)(const char *dsn),
    const kvspace_static_sym_t *syms, uint32_t nsyms);

#ifdef __cplusplus
}
#endif
#endif
