# malloc benchmark harness

对 `slotsboxmalloc` 与通用分配器做对照测量。结果只进 `result/csv/`，
列约定见 [`result/csv/SCHEMA.md`](../result/csv/SCHEMA.md)。

## 跑法

```bash
# 全量（默认，耗时长）
python3 benchmark/tools/run_malloc_bench.py

# 冒烟：缩放 workload 规模，用于验证 harness 本身
python3 benchmark/tools/run_malloc_bench.py --scale 0.01
```

`--scale` 只缩 workload 规模，不改尺寸档与 pattern 覆盖。**正式结果必须用默认 1.0**。

依赖：

- `slotsboxmalloc` 仓在兄弟目录（可用 `SLOTSBOXMALLOC_ROOT` 覆盖），且已 `make`；
- `libblockmalloc` 走 header-only，只需头文件目录存在；
- `taskset`（单线程固定到 0 号核）。

## 产物

```
result/csv/results-malloc-<commit短哈希>-<YYYYMMDDTHHMMSSZ>.csv
result/csv/results-malloc-<commit短哈希>-<YYYYMMDDTHHMMSSZ>.meta.json
```

两者都由脚本生成，**数值不手改**。CSV 头部两行 `#` 注释给出生成脚本、时间戳、
版本、编译选项；`meta.json` 另含完整编译命令、运行命令、宿主机信息与缺失库清单。

读 CSV 时跳过注释行：

```python
pandas.read_csv(path, comment="#")
```

## 公平性约定：所有实现走同一 shim

被测实现只有一个接口面：`alloc(size) -> ptr` 与 `free(ptr)`。
`malloc_bench.c` 里的 `shim_t` 把各实现适配到这个面，**不做实现特化的预热、
批处理或缓存**。任何实现都不允许走与自己场景不匹配的捷径。

## 语义差异（必须读，否则会误读数字）

这不是「同一个东西的两种实现」，两者的前提不同：

| 维度 | slotsboxmalloc | glibc malloc |
|---|---|---|
| 工作区域 | **预划的连续区域**（meta 区 + data 区），创建后不增长 | 向 OS 按需 `brk`/`mmap` 要页 |
| 返回物 | 区域内**字节偏移**，shim 转成 `base + offset` | 直接是虚拟地址指针 |
| arena | **单 arena**（`root_slots == 1` 时）；默认配置下按数据区大小分 64 个 shard | 多 arena，按线程分散 |
| 单次上限 | shard 的 **63/64**，超过即失败 | 受地址空间限制 |
| 元数据位置 | 与数据区**分离**，全在 meta 区 | 与对象相邻（chunk header） |
| 碎片整理 | 不做（见论文第 4 章） | 不做 |

因此：

- **`fragmentation_ratio` 只有 slotsbox 有**。它的口径是
  「区域内空闲 slot 总数 / 最长连续空闲段」，来自 slotsbox 自己的位图。
  glibc 没有等价量，本 harness **不编造**这一列。
- **`reserved_bytes` 含义不同**：slotsbox 是预划区域总量（固定）；
  glibc 是进程 `VmSize`（随分配增长）。两者不可直接比较。
- **失败要如实看 `notes`**：slotsbox 在尺寸超过 63/64 shard 上限时会返回失败，
  harness 在 `notes` 写 `failures=N/M`，不用「成功的那些」冒充全部。

## 对照库：jemalloc / mimalloc / tcmalloc

三个库**运行时 `dlopen`**，不直接链接。理由：直接链接要为每个库单独编一份
harness，三份二进制编译选项稍有差异就是三种口径；dlopen 让三者与 slotsbox/glibc
走同一个二进制、同一套计时循环。

### 用的入口：标准 `malloc` / `free`

三个库**都导出标准的 `malloc`/`free`**（已 `nm -D` 核实），所以 harness 不需要
按库特化入口。**不**用 tcmalloc 的 `tc_malloc` 之类的前缀入口——那是真实 C 代码
不会走的名字，测它就等于测一条假路径。

### jemalloc 的 TLS 限制（重要）

`dlopen("libjemalloc.so.2")` 在本机**失败**：

```
cannot allocate memory in static TLS block
```

这是 jemalloc 的已知限制——它用大量静态 TLS，进程启动后再 dlopen 装不下；
必须在启动前 `LD_PRELOAD` 或直接链接。**实测确认**：

```
$ dlopen(RTLD_LOCAL)  → cannot allocate memory in static TLS block
$ dlopen(RTLD_GLOBAL) → 同样失败
$ LD_PRELOAD=libjemalloc.so.2 <同一程序>  → 成功
```

所以当前 `jemalloc` 的 CSV 行是 `skipped: ... cannot allocate memory in static
TLS block`，**不是「没装」**。要拿到 jemalloc 数据，跑法必须改成：

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libjemalloc.so.2 \
  python3 benchmark/tools/run_malloc_bench.py
```

但那样 jemalloc 会**全局接管 malloc**，slotsbox/glibc 两列的数字也会被污染——
所以这不是简单加一个环境变量的事，需要把 jemalloc 单独跑一轮并标明「本轮全进程
使用 jemalloc」。**当前版本不这么做**：宁可留 skipped，也不产出混了口径的数字。

### skipped 行

库未安装、dlopen 失败、或缺少 `malloc`/`free` 导出时写

```
malloc,<lib>,-,ops_per_second,-,ops/s,n/a,<ts>,skipped: <原因>,...
```

**不伪造行**，且 `notes` 必须写清具体原因（缺 soname / TLS 失败 / 导出缺失），
三种原因的可比性完全不同。

## 多线程

当前 harness **只跑单线程**（`taskset -c 0`）。多线程结果不在这里产生，
也不会用单线程数字冒充多线程——需要时另开一轮并把 `threads` 列写实数。

## 跨进程

跨进程共享内存场景**不在 malloc suite 范围内**（那是 kvspace 层的 benchmark）。
见 `result/csv/SCHEMA.md` 的 `kvspace` suite。

## 已知的测量坑

1. **陈旧 .so**。`slotsboxmalloc/build/libslotsboxmalloc.so` 可能是几个月前的产物，
   与当前头文件的结构体布局不匹配，症状是 `sbo_init` 返回 0 但
   `root_slots=0`、`block_stride=0`（初始化成功却写出非法状态）。
   **跑之前先 `make`**；`sbo_meta_size()` 的返回值可以当作快速体检——
   128MB 数据区应返回约 16.7MB（64 shard），若返回 262320 就说明 .so 是陈旧的。

   本 harness 有两道防护：runner 在**开始前**比较 `.so` 与头文件的 mtime，
   `.so` 更旧就直接拒绝运行；跑完把 `.so` 与头文件的 **路径、mtime、size、sha256**
   写进 `meta.json` 的 `tested_library` / `header` 两栏。事后核对结果时先看这两栏，
   能立刻排除「拿旧库出的数」。
2. **`sbo_init` 不能在同一块 meta 区上重跑**（magic 命中即失败）。
   harness 在每个尺寸档前 `memset` 整个 meta 区，模拟「全新区域」。
3. **吞吐取 5 轮里最快的一轮**，延迟取同一轮的 p50/p99。噪音只会让某一轮变慢，
   取最小是稳健的；但这也意味着**结果是上界**，不是平均值。
