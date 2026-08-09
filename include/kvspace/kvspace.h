/*
 * kvspace.h — KVSpace C API
 *
 * file-backed mmap, ART 树索引 + slotsboxmalloc 变长存储.
 * 对齐 kvspace-go KVSpace 接口.
 */

#ifndef KVSPACE_H
#define KVSPACE_H

#include "kvspace/xvalue.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct kvspace kvspace_t;

/* ================================================================
 * 生命周期
 * ================================================================ */

// 打开或创建 SHM。data_size 必须为 64 的幂 × 8 的倍数.
kvspace_t *kvspace_open(const char *path, size_t data_size);
void       kvspace_close(kvspace_t *kv);

/* ================================================================
 * 单点读写
 * ================================================================ */

// Get: 返回 malloc 的 TLV buffer，调用者 free。缺失返回 NULL, *out_len=0.
uint8_t *kvspace_get(kvspace_t *kv, const char *key, int32_t *out_len);

// Set: 写入 value（TLV 编码的字节）。自动维护目录索引。
// 父目录不存在则失败。
int kvspace_set(kvspace_t *kv, const char *key, const uint8_t *val, int32_t val_len);

/* ================================================================
 * 目录操作
 * ================================================================ */

// List: 返回直接子项名数组 + 个数。调用者 free(*out_names) 和每个元素。
int kvspace_list(kvspace_t *kv, const char *prefix, bool expand_ext,
                 char ***out_names, int32_t *out_count);

int kvspace_del(kvspace_t *kv, const char *key);
int kvspace_deltree(kvspace_t *kv, const char *prefix);
int kvspace_mkindex(kvspace_t *kv, const char *path); // 递归创建目录

/* ================================================================
 * Link / ExtIndex
 * ================================================================ */

int kvspace_link(kvspace_t *kv, const char *target, const char *linkpath);
int kvspace_extindex(kvspace_t *kv, const char *path, const char *extpath);
int kvspace_unlink(kvspace_t *kv, const char *path);

/* ================================================================
 * Watch / Notify
 * ================================================================ */

int kvspace_notify(kvspace_t *kv, const char *key, const uint8_t *val, int32_t val_len);

// 阻塞等待通知，timeout_ms 毫秒。返回 malloc TLV，超时返回 NULL.
uint8_t *kvspace_watch(kvspace_t *kv, const char *key, int32_t timeout_ms, int32_t *out_len);

#endif
