# kvspace

KVSpace 的存储层实现：一个 `.so`，两个层次，互不越界。

| 层 | 头 | 符号 |
|---|---|---|
| 存储 ABI | `kvspace.h` | 13 个：Connect / Close / XvalueInProcessAddress / ResolveOffset / AnchorOf / Realloc / Free / Cp / Mkindex / Deltree / Cpdir / Listlen / List |
| 字节层 | `kvspace_xvalue.h` | 3 个：XvalueGetAttr / XvalueSetAttr / XvalueHeadEffectiveLen |
| 存储常量 | `const.h` | 1 个：`kvspaceConstStorage` |

**语言层不在这里**：langtype（tag 1）、wire 构造器、坐标段比较器、kind 常量
都在 kvlang 仓的 `libkvlang_xvalue.so`（`kvlangxvalue/kvlangxvalue.h`，常量查询在同头声明）。
**本仓不导出任何 kind 常量**——`const.h` 只有 2 个存储常量（目录索引后缀、成员分隔符），
语言层的 `KVLANG_KIND_*` 一个都不在这里。
kvspace 是存储层，它不认识 langtype 这个词；把语言概念写进存储头，等于让存储层依赖
语言层——那样的 kvspace 就不能被别的语言复用，也不能被独立替换。

## 值在存储层看来是什么

一条 XValue wire 对 kvspace 只是**字节**：

```
[bodylen:u64 LE@0][attrslen:u16 LE@8][padding:6@10-15]
[body @16..16+bodylen][attr @16+bodylen..]

record = [tag:u8][len:u16 LE][data]；attr 区无长度前缀（长度只在 head 里）
```

`tag 1` 在这里只是一个编号，内容对存储层不透明。唯一的例外是 tag 2（已退役的
reallen）：**任何读写路径见到它都必须拒**——那是「写读双方对格式理解不一致」的证据，
属线格式契约而非语言语义。见 `kvspace_xvalue.h` 的 `GetAttr`/`SetAttr` 注释。

## 常量

`include/kvspace/const.h` 是**存储**常量的唯一定义处：目录索引后缀 `/`、成员分隔符
`·`、两条错误串。`kvspaceConstStorage("KVSPACE_MEMBER_SEP")` → `"·"`，供
Go/Python/Rust 扩展运行期取，免去硬编码裸字面量。运行期成员分隔符 `‥` 属于语言层，
已移到 `kvlangConstLanguage("KVLANG_RUNTIME_MEMBER_SEP")`。

语言常量（`int64`/`char/utf8`/`stringkeymap`/`rwir`/…）走另一个入口
`kvlangConstLanguage`——同一个函数返回两种层的东西，等于把「这个名字属于哪层」
重新变成调用方要猜的问题。

## List 的顺序

`kvspaceList` **按后端顺序返回、不排序**，这是契约，不是实现细节。坐标序（`[2]`
排在 `[10]` 前面）是上层的事：需要它的调用方用 `kvlangCoordCmp`（唯一一份
`cmp_coord`），不要指望存储层给。

## 公共 ABI 约定

- **输入是前置条件**：`handle`、`key`、必须的输出指针不得为 `NULL`；违反是
  undefined behavior，不返回错误码。返回码只描述存储语义。
- **没有错误串**：接口不接收 `(char *err, uint32_t cap)`。失败原因是返回值里的
  负 errno；调用方自己知道 key 与操作，要日志就按这两项拼。
- 返回码：`OK=0`；`INVALID=-EINVAL`、`MISSING=-ENOENT`、
  `ANCHOR_INVALID=-ESTALE`、`CAPACITY=-ENOBUFS`、`NOT_INDEX=-ENOTDIR`、
  `HAS_CHILDREN=-ENOTEMPTY`、`BACKEND=-EIO`。
- **key 语法**：绝对 key 以 `/` 开头；分隔符只有 `/` 与 `·`；末字节是分隔符的
  key 是目录（索引），否则是值；相邻分隔符、长度 ≥1024 一律 `INVALID`。
- **anchor**：`NULL` 表示按绝对 key 从根解析；非空表示从 `node_offset` 指向的
  目录节点按**相对后缀**解析，key 不得再以 `/` 开头。`epoch` 用于判断是否需要
  跟随迁移，`flags` 必须含 `KVSPACE_ANCHOR_FLAG_INDEX`；节点已删除或标志不符
  返回 `ANCHOR_INVALID`。并发结构变化下解析最多重试 3 轮；仍不稳定同样返回
  `ANCHOR_INVALID`，调用方重新 `kvspaceAnchorOf` 获取即可。
- **输出有效条件**：`ResolveOffset`/`Realloc` 只在返回 `OK` 时写 `out_value_offset`；
  `Listlen` 先清零再写；`List` 的 `out_length` 是 NUL 分隔名字的总字节数，
  `out_names == NULL` 时只查询不写，容量不足返回 `CAPACITY` 且 `out_length`
  给出所需字节数。`out_count`/`out_generation` 可为 `NULL`。
- **并发**：偏移与值字节都在同一张共享映射里，跨进程直接寻址；删除是延迟回收
  （退役环），已拿到的偏移在返回值有效期内可读。同一 key 的并发写由调用方串行化。

## 兼容性

本分支有三类不兼容变化，旧环境必须重建或同步升级：

- **shm store 布局**：共享头字段、退役环侧车 `.retirement` 与旧版本不同；旧 store
  不能打开。magic 不承载版本号，按「只有一个 version、不向后兼容」处理：删除旧
  store 后重建。
- **`.gen` 旁路文件删除**：节点级 generation 是只写不读的死状态，连同
  `generations_grow` / `generations_at` 一并移除；旧 store 遗留的 `.gen` 文件可以
  直接删掉。anchor 用的版本号改由共享头的 `structure_epoch` 承担。
- **anchor ABI**：`kvspaceResolveOffset` 的 anchor 参数由整数改为
  `const kvspace_anchor_t *`。`NULL` 表示从根按绝对 key 解析；非零 anchor 表示从
  该目录节点按相对后缀解析。跨仓调用方必须同步声明与调用点。
- **常量归属**：`KVSPACE_PATH_SEP` 删除（与 `KVSPACE_DIR_INDEX_SUF` 重复）；
  `KVSPACE_RUNTIME_MEMBER_SEP` 移到 `kvlangConstLanguage("KVLANG_RUNTIME_MEMBER_SEP")`；
  两条 `KVSPACE_ERR_*` 删除——接口不再接收错误缓冲，错误信息由调用方按 return code
  与自己的操作上下文组织。
- **错误缓冲**：10 个存储符号删除 `(char *err, uint32_t cap)` 形参；输入指针改为
  前置条件；`KVSPACE_CAPACITY` 的 errno 从 `ENOSPC` 改为 `ENOBUFS`。

合并顺序：kvlang 的常量迁移与 anchor ABI 迁移先落地，再合并 kvspace，避免 kvlang
的扩展与绑定点查询已删除的常量或按旧参数调用。

## Build

```bash
# 生产（17 个导出符号：13 存储 + 3 字节层 + 1 常量；内部符号隐藏）
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build -j

# 测试（额外打开内部观测钩子，并把 kvspaceTest* 放出来供测试直接链接）
cmake -S . -B build-test -DBUILD_TESTING=ON
cmake --build build-test -j && ctest --test-dir build-test
```

**本仓不依赖 kvlang**：测试造值用 `tests/shm/test_wire.h` 里的本地 inline 拼 wire，
不链 `libkvlang_xvalue.so`；CLI 只做存储子命令，也只链 `libkvspace`。所以上面两条
命令都不需要外部路径参数。谁要构造带 langtype 的值，谁就是语言层的消费者——那是
kvlang 仓的 `tutorial-c/` 与 `bindings/{py,rust}/`。

## Install

```bash
cmake --install build                   # libkvspace.so.1 + include/ + kvspace.pc
```

## 目录

```
include/kvspace/kvspace.h          # 权威存储 ABI 头（13 符号）
include/kvspace/kvspace_xvalue.h   # 字节层：head 布局 + attr 读写（3 符号）
include/kvspace/const.h            # 存储常量 + kvspaceConstStorage()
src/kvspace.c                      # SHM 引擎（file-backed mmap + ART）
src/xvalue_head.c                  # 字节层实现
src/const.c                        # 存储常量查找表
cli/                               # kvspace CLI（Cargo）——只做存储子命令
tests/                             # 存储契约 + 字节层
```

**不在本仓**（语言层示例与绑定都在 kvlang）：`tutorial-c/`、`py/`、`rust/`。
它们要用 wire 构造器与 langtype 访问器，属语言层；留在这里就是 kvspace 依赖
kvlang，方向反了。见 `array2d/kvlang/tutorial-c/`、`kvlang/bindings/{py,rust}/`。
