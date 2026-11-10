# kvspace

KVSpace 的存储层实现：一个 `.so`，两个层次，互不越界。

| 层 | 头 | 符号 |
|---|---|---|
| 存储 ABI | `kvspace.h` | 12 个：Connect / Close / XvalueInProcessAddress / Resolve / Realloc / Free / Cp / Mkindex / Deltree / Cpdir / Listlen / List |
| 字节层 | `kvspace_xvalue.h` | 3 个：XvalueGetAttr / XvalueSetAttr / XvalueHeadEffectiveLen |
| 存储常量 | `const.h` | 1 个：`kvspaceConstStorage` |

**语言层不在这里**：langtype（tag 1）、wire 构造器、坐标段比较器、20 个 kind 常量
都在 kvlang 仓的 `libkvlang_xvalue.a`（`include/kvlang/xvalue.h`、`const.h`）。
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

`include/kvspace/const.h` 是**存储**常量的唯一定义处：路径分隔符 `/`、成员分隔符
`·`、运行期成员分隔符 `‥`、两条错误串。`kvspaceConstStorage("KVSPACE_MEMBER_SEP")`
→ `"·"`，供 Go/Python/Rust 扩展运行期取，免去硬编码裸字面量。

语言常量（`int64`/`char/utf8`/`stringkeymap`/`rwir`/…）走另一个入口
`kvlangConstLanguage`——同一个函数返回两种层的东西，等于把「这个名字属于哪层」
重新变成调用方要猜的问题。

## List 的顺序

`kvspaceList` **按后端顺序返回、不排序**，这是契约，不是实现细节。坐标序（`[2]`
排在 `[10]` 前面）是上层的事：需要它的调用方用 `kvlangCoordCmp`（唯一一份
`cmp_coord`），不要指望存储层给。

## Build

```bash
# 生产（16 个导出符号：12 存储 + 3 字节层 + 1 常量；内部 kvspaceShm* 隐藏）
cmake -S . -B build -DBUILD_TESTING=OFF
cmake --build build -j

# 测试（额外打开内部观测钩子，并把 kvspaceShm* 放出来供测试直接链接）
cmake -S . -B build-test -DBUILD_TESTING=ON
cmake --build build-test -j && ctest --test-dir build-test
```

**本仓不依赖 kvlang**：测试造值用 `tests/shm/test_wire.h` 里的本地 inline 拼 wire，
不链 `libkvlang_xvalue.a`；CLI 只做存储子命令，也只链 `libkvspace`。所以上面两条
命令都不需要外部路径参数。谁要构造带 langtype 的值，谁就是语言层的消费者——那是
kvlang 仓的 `tutorial-c/` 与 `bindings/{py,rust}/`。

## Install

```bash
cmake --install build                   # libkvspace.so.1 + include/ + kvspace.pc
```

## 目录

```
include/kvspace/kvspace.h          # 权威存储 ABI 头（12 符号）
include/kvspace/kvspace_xvalue.h   # 字节层：head 布局 + attr 读写（3 符号）
include/kvspace/const.h            # 存储常量 + kvspaceConstStorage()
src/shm/kvspace.c                  # SHM 引擎（file-backed mmap + ART）
src/xvalue_head.c                  # 字节层实现
src/const.c                        # 存储常量查找表
cli/                               # kvspace CLI（Cargo）——只做存储子命令
tests/                             # 存储契约 + 字节层
tutorial-durable/                  # durable 后端 tutorial
```

**不在本仓**（语言层示例与绑定都在 kvlang）：`tutorial-c/`、`py/`、`rust/`。
它们要用 wire 构造器与 langtype 访问器，属语言层；留在这里就是 kvspace 依赖
kvlang，方向反了。见 `array2d/kvlang/tutorial-c/`、`kvlang/bindings/{py,rust}/`。
