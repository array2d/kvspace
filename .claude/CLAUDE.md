# CLAUDE.md

## 定位

kvspace-c 是文件系统风格 KV 存储的 C 实现。ART 树做 key 索引，slotsboxmalloc 做 value 变长存储，file-backed mmap 做持久化 + 多进程共享。

依赖：blockmalloc + slotsboxmalloc（均为 header-only + .so 双模库）。

## API

```c
kvspace_t *kvspace_open(const char *path, size_t data_size); // data_size = 8×64^k
void       kvspace_close(kvspace_t *kv);
uint8_t   *kvspace_get(kvspace_t *kv, const char *key, int32_t *out_len);
int        kvspace_set(kvspace_t *kv, const char *key, const uint8_t *val, int32_t val_len);
int        kvspace_del(kvspace_t *kv, const char *key);
int        kvspace_deltree(kvspace_t *kv, const char *prefix);
int        kvspace_mkindex(kvspace_t *kv, const char *path);
int        kvspace_list(kvspace_t *kv, const char *prefix, bool expand_ext, char ***names, int32_t *count);
int        kvspace_link(kvspace_t *kv, const char *target, const char *linkpath);
int        kvspace_extindex(kvspace_t *kv, const char *path, const char *extpath);
int        kvspace_unlink(kvspace_t *kv, const char *path);
int        kvspace_notify(kvspace_t *kv, const char *key, const uint8_t *val, int32_t val_len);
uint8_t   *kvspace_watch(kvspace_t *kv, const char *key, int32_t timeout_ms, int32_t *out_len);
```

## 架构

### SHM 布局（单 mmap 块）

```
[kvspace_hdr_t][blocks_meta_t×1][ART slab (256MB)][sbo meta][sbo data]
```

- ART 树节点：1 个 blockmalloc 实例管理固定大小块（2112B = N256）
- slotsboxmalloc：管理变长 value（XValue TLV bytes）
- sbo 内部再用 1 个 blockmalloc 管理 box_head_t 节点

### ART 树

4 种自适应节点（N4/N16/N48/N256），前缀压缩，零指针（用 block_id）。所有节点在同一 slab。

核心操作：`art_ins`（含 prefix split + grow）、`art_search`、`art_del`、`art_scan`（前缀扫描供 List 用）。

**已知限制**：同名 key 的有/无尾斜杠共存未实现 prefix split（如 `/a` 和 `/a/` 同时存在时 Get("/a") 返回 NULL）。

### XValue TLV 格式

```
[1B kind_len][N B kind][4B arraylen LE][4B raw_len LE][M B raw]
```

kind 常量：`""(none)` `"int64"` `"float64"` `"string"` `"index"` `"linkindex"` `"extindex"` 等 17 种。

每个类型天然支持数组（arraylen 标识元素数）。对齐 kvspace-go 的 XValue 接口。

### Index / Link / ExtIndex

- **Index**：目录标记。kind="index", raw=""。List 通过 ART 前缀扫描获取子节点，不依赖 index value 的内容。
- **LinkIndex**：kind="linkindex", raw=target 路径。当前仅存储，Get/List 未穿透。
- **ExtIndex**：kind="extindex", raw="…"+extpath。当前仅存储，List 未展开 extpath 子节点。

### Watch/Notify

per-key pthread condvar，hash 分桶（256 槽），timeout 支持。

## 构建与测试

```bash
make                 # Release 构建 → build/libkvspace-c.so
make clean
```

```bash
python3 tutorial/test.py   # 多语言测试（C/C++/Python/Rust 共 11 个 case）
```

测试用 Python ctypes 调 .so，纯 Python TLV 编码（不依赖 C xvalue 构造函数）。

## 多语言 wrapper

```
py/kvspace/          ← Python ctypes 绑定（pip installable）
rust/                ← Rust FFI crate（Cargo）
```

C++ 直接 `extern "C"` include 头文件，无需独立包装。

## 文件结构

```
include/kvspace/xvalue.h   ← XValue 类型系统（17 种 kind, TLV 编解码）
include/kvspace/kvspace.h  ← KVSpace API 声明
src/xvalue.c               ← XValue 实现
src/kvspace.c              ← SHM + ART + KV 操作 + Watch/Notify
tutorial/                  ← 多语言测试用例 + test.py
py/                        ← Python ctypes wrapper
rust/                      ← Rust FFI crate
```
