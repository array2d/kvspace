# kvspace

KVSpace 的 dispatch 前端。消费者只链接 `libkvspace`，运行期按 DSN scheme 选择后端：

| DSN scheme | 后端 | 装载名 |
|---|---|---|
| `shm://...` | kvspace-c（file-backed mmap + ART） | `libkvspace-c.so.1` |
| `redis://` `fs://` `s3://` | kvspace-durable（Rust） | `libkvspace_durable.so.1` |

两个后端导出同一套 25 个 `kvspace*` C ABI（见 `include/kvspace/kvspace.h`）。
前端用 `dlopen(RTLD_NOW | RTLD_LOCAL)` 装载后端，handle 包一层 vtable；
codec（`kvspaceTlvEncode*`/`kvspaceDecodeHead`/`kvspaceNew*`）无 handle，由前端静态实现，
head 格式 byte-identical，两个后端与前端三者共用同一份契约。

后端查找目录由环境变量 `KVSPACE_BACKEND_PATH` 覆盖，默认走动态链接器搜索路径。

## Build

```bash
cmake -B build && cmake --build build   # 产出 libkvspace.so.1
```

## Install

```bash
cmake --install build                   # libkvspace.so.1 + include/ + kvspace.pc
```

## 目录

```
include/kvspace/kvspace.h   # 权威 C ABI 头
src/frontend.c              # dispatch 前端 + 静态 codec
cli/                        # kvspace CLI（从 kvspace-durable 迁入）
tutorial-c/                 # kvspace-c（SHM）多语言 tutorial
tutorial-durable/           # kvspace-durable（redis/fs/s3）tutorial
```

