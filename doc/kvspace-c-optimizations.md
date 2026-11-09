# kvspace-c 当前优化加速技术调研

- 快照：`kvspace@80e6d4e`、`kvspace-c@dcb5b68`、`slotsboxmalloc@b871d48`、`blockmalloc@43f57a6`
- 范围：`kvspace-c` 的 shm 后端，以及它直接依赖的 `slotsboxmalloc` / `blockmalloc`
- 只记**当前代码里在用**的技术；实验分支与未合并方案单列，不算现状
- 判据：每条给「机制 / 位置 / 效果（有测量才写）/ 边界」。跨进程合法性单列（见 §十）

## 〇、结论

优化集中在四层：

1. **键索引**：ART（自适应节点 + 路径压缩 + MOVED 转发 + 30 位 block id）；
2. **值存储**：slotsboxmalloc（64 叉伙伴树 + free_mask + 容量提示 + 热缓存 + 分片锁）；
3. **内存布局与增长**：三段独立映射 + 预留 VA + `MAP_FIXED` 原地增长 + 偏移不变；
4. **并发与回收**：单写者契约、每分片锁、P2 进程注册/屏障、代次表、垃圾环。

**跨进程判据**：不是"实现是否进程本地"，而是"产出的东西是否跨进程可解释、失效是否跨进程可见"。
偏移（ART block id、sbo offset）、shm 内元数据、代次是合法的；**裸指针不是**。
因此 `kvspaceXvalueBodyView` 与 `kvspaceViewToken_t.head/body` 列为**非法设计，待删除**（§十一）。

## 一、键索引：ART

| 技术 | 机制 | 位置 |
|---|---|---|
| 自适应节点 | `N4` / `N16` / `N48` / `N256` 四种节点，按键基数升级 | `kvspace-c/src/kvspace.c:58-105` |
| 路径压缩 | 节点头带 `prefix[10]` + `prefix_len`，公共前缀只存一次 | `:64-71`、`:1570-1595` |
| 子查找 | N4/N16 线性扫 `keys[]`，N48 用 256 字节 `index[]`，N256 直接数组 | `:1532-1565` |
| 节点升级 | 旧节点置 `ART_MOVED`，`val` 存新节点 id；`art_follow` 最多 8 跳 | `:1682-1694`、`:1724-1814` |
| 30 位 block id | 节点 id 是 ART slab 的块号；`blockdata_offset = id × BLOCK_SIZE + head` | `:35-46`、`blockmalloc/include/blockmalloc/blockmalloc.h:278` |
| 追加式分配 | 节点池只追加、id 不复用；`art_bump` 连续块 | `:1434-1517` |
| 前缀/父节点复用 | `ResolveRef` 返回 `block_id + parent_id + depth`，后续从父节点续走 | `:2882-2891`、`:1570` |

**效果**：查找 O(key 长度)，无 rehash；路径压缩减少节点数。
**边界**：节点只追加不释放，键频繁增删会让 ART slab 单调增长；`ART_MOVED` 链最多 3 次升级（N4→N16→N48→N256），8 跳上限有富余。

## 二、值存储：slotsboxmalloc

| 技术 | 机制 | 位置 |
|---|---|---|
| 64 叉伙伴树 | 数据区 `8 × 64^k` 字节；每 box 64 slot，子 box 左分配、对象右分配 | `slotsboxmalloc/include/slotsboxmalloc/slotsboxobj.h:99-131` |
| 容量编码 | `enc = (level<<6) | multiple`，可直接整数比较 | 同上 |
| free_mask | 单个 `uint64_t` 位图，`clz`/`ctz` 定位连续空闲段 | `:310-342`、`:388-389` |
| 容量提示 | 父 box 的 `child_cap[]` 缓存子 box 容量；`child_max_enc` + `child_max_count` 沿父链 O(1) 传播 | `:214-245` |
| 热缓存 | 每 shard 锁内缓存 L1/L2/L3 活跃 box 的 `hot_id1` / `hot_base`，命中 O(1) 直达 | `:55-56`、`:266-296` |
| 分片 | 数据区 ≥ 64×2MB 时切 64 shard，每 shard 独立锁、独立 blockmalloc 池、独立树根 | `:99-131`、kvspace-c `:10` |
| 锁 | 每 shard TTAS spinlock；TLS 亲和 home shard；先 trylock 轮转、再阻塞轮转 | slotsboxmalloc `CLAUDE.md`、`:606-650` |
| 惰性回收 | 空子 box 保持温热；父层不足或 meta 块耗尽时 `sbo_reclaim_empty` 回收全空子树 | `:279-296`、`:380-382` |

**效果**：分配/释放接近 O(1)，多线程按 shard 扩展，锁竞争不误报 OOM。
**边界**：单次分配上限约 shard 的 63/64；meta 太小会退回单 shard；TLS 亲和可能造成 shard 间不均。

## 三、内存布局与增长

| 技术 | 机制 | 位置 |
|---|---|---|
| 三段独立文件/映射 | `<path>` 存 ART slab，`.sbo.head` 存 box 元数据，`.sbo.data` 存值体 | `kvspace-c/src/kvspace.c:143-170` |
| 预留 VA + 原地增长 | `mmap(NULL, reserve, PROT_NONE)` 预留，再 `MAP_FIXED` 映射；基址进程内不变 | `:259-286` |
| ART 增长 | slab 2×；`ftruncate` + `region_map` | `:1280-1286` |
| sbo head 增长 | 2×，受 block-head 位宽上限约束 | `:1312-1330` |
| sbo data 增长 | ×64；根节点上移一层，**已有 offset 不变** | `:1335-1350` |
| 跨进程扩容同步 | `kv_sync` 按 header 里的新尺寸重映射，别的进程扩过也能跟上 | `:1267-1279` |
| 归还磁盘 | Linux 对 ≥64KB 释放做 `PUNCH_HOLE`；macOS 跳过 | `:1419-1428` |

**效果**：扩容不搬移已有值，偏移长期有效；这是"偏移跨进程可解释"的基础。
**边界**：依赖预留 VA（ART 2^36、data 2^40）；预留耗尽无法再增长。

## 四、写路径

| 技术 | 机制 | 位置 |
|---|---|---|
| 已存在 box 原地覆盖 | 新长度 ≤ 当前 box 容量时直接 `memcpy` 进原 box，前后各 bump 一次 `value_gen`（seqlock） | `kvspace-c/src/kvspace.c:3139-3155` |
| 预留容量 | `WriteNewPlace` 按 `body_cap` 预留物理 box，body 后续可在原地长 | `:3462` 附近 |
| SetPart 快路径 | 直读 head + 校验 per-box 物理容量后写正文；不自洽则回退 | `:2934-2950` |
| 垃圾环 | 换 box 前 `garb_reserve`，成功 `garb_commit`、失败 `garb_cancel`；退役点不拿分配器锁 | `:395-440`、`:422` |
| 异步回收 | 累计退役字节 ≥ 64MiB 触发维护线程；单写者写 head/tail | `:133`、`:353-363` |

**测量**（`bench/results/2026-10-08-existing-writes`，Xeon Gold 6418H，13 对交替，单位 ns/setter）：

| 场景 | main | 候选 | 加速 |
|---|---:|---:|---:|
| 单 key 覆盖 | 160.446 | 112.875 | **1.42×** |
| 128 key 覆盖 | 208.093 | 166.941 | **1.25×** |

**边界**：只对"已存在且长度不增"的写有效；新 key 插入仍是分配瓶颈（对照：短 key +0.27%、长 key −0.19%）。

## 五、读路径与零拷贝

| 技术 | 机制 | 位置 |
|---|---|---|
| 借用读 | `kvspaceGet` 直接返回 mmap 内指针，不拷贝 | `kvspace-c/README.md`、`kvspace.c:2707` |
| ref 续查 | `ResolveRef` 缓存 `block_id/parent_id/depth`；`GetByRef`/`SetPartByRef` 从父节点续走，省一次全路径解析 | `:2855`、`:2909` |
| 版本化 view token | `struct_gen`（结构/搬迁）+ `value_gen`（值写/删，seqlock）+ `store_id`/`node_id` | `kvspace/include/kvspace/kvspace.h:119-131`、`kvspace.c:650-720` |
| quiesce/ack | 视图使用前 `Quiesce`，屏障轮次；fork 后旧 token 失效 | `:640-650`、`:900-1000` |

**边界（重要）**：`kvspaceXvalueBodyView` 和 `kvspaceViewToken_t` 的 `head/body` 是**裸指针**，
只在本进程映射内有效；跨进程不可解释。按本调研的判据，这是非法设计，见 §十一。

## 六、目录枚举

| 技术 | 机制 | 位置 |
|---|---|---|
| 直接子项截断 | 扫到 child 分隔符就停，只 probe 该节点是否有 live value，不物化所有后代路径 | `:2247-2330` |
| 每 handle List 缓存 | 缓存 `(prefix, ex, resolve, target, ext)` + `list_gen`；ListLen 与 ListAt 共享一次扫描 | `:3894-3960` |
| 跨进程失效 | 任何 key 出现/消失都 bump `hdr->list_gen`；缓存命中要求代次不变 | `:164-168`、`:254`、`:869`、`:3204` |

**测量**（`bench/README.md`，9 对交替，单位 µs/call）：

| 场景 | main | 候选 | 加速 |
|---|---:|---:|---:|
| 10k 后代 / 1 子项 | 2439.481 | 0.101 | **24,187×** |
| 10k 后代 / 10 子项 | 2689.760 | 1.078 | **2,496×** |
| 10k 平铺子项 | 2489.855 | 1903.179 | **1.31×** |
| 9,999 删除 / 最后值存活 | 106.534 | 50.557 | **2.11×** |

**边界**：平铺 10k 仍要遍历，只有 1.3×；全墓碑分支仍要访问节点；长名性能取决于树形。

## 七、并发与多进程

| 技术 | 机制 | 位置 |
|---|---|---|
| 单写者契约 | 同一 store 同一时刻只有一个写者进程；`committed_bytes` 等只由写者写 | `:162`、`:404`（kvspace-c #58） |
| 原子 ART 值 | `val` 的 acquire/release/CAS，`has_value + box_offset` 单字 | `:73-85` |
| 进程注册表 | 64 槽，`pid + starttime` 双重判活；claim id 防 pid 复用 | `:118-170`、`:500-590` |
| 恢复屏障 | leader 选举（单词 CAS）、等待各槽 ack、quiesce | `:900-1000` |
| 代次表 | 每 ART 节点 8B：`struct_gen` + `value_gen`；分配时取全局 epoch | `:603`、`:331-345` |
| fork 守卫 | `pthread_atfork` 子进程钩子把全局世代 +1；热路径只比两个 u32，不调 `getpid` | `:629-640` |

**边界**：单写者限制写吞吐扩展；多写者需要另一套协议（当前明确不做，见 #58）。

## 八、编码层

| 技术 | 机制 | 位置 |
|---|---|---|
| 16 字节定长 head | `body` 恒在 `head + 16`；单 key 一次写完整 XValue | `kvspace/include/kvspace/xvalue_head.h`、`kvspace.c:3461` |
| attr 记录 | `tag 1 = langtype`，`tag 2 = reallen`；无前缀，`attrslen` 在 head | 同上 |
| 无 storetype/ptr 字段 | 存储形态与引用性从 langtype 前缀 (`*`/`@`) 推出 | `kvspace-c/README.md` |
| 小值内联 | 定长小值直接存 body；大值走 box | `:3462`、`xvalue_head.h` |
| 格式版本 | 当前 store format **7** | `src/kvspace_shm.h:28` |

## 九、测量证据汇总

| 来源 | 内容 | 备注 |
|---|---|---|
| `bench/results/2026-10-08-existing-writes` | 覆盖写 1.42× / 1.25× | 后端微基准，非端到端 |
| `bench/results/2026-10-08` | 直接子项枚举 24,187× / 2,496× / 1.31× / 2.11× | 同上 |
| `README.md` | 两套 API、16B head、格式版本 | 文档 |

所有数字都来自上表目录的样本与 metadata；跨版本比较前先核对编译器、CPU、依赖版本。

## 十、跨进程合法性判定

判据：**产出的东西是否跨进程可解释；失效是否跨进程可见。**

**合法**：

- 偏移：ART block id、sbo box offset（区相对，同一 store 跨进程同一值）；
- shm 内元数据：`gen` 表、`list_gen`、分片锁、P2 槽、垃圾环、`art_root`；
- 代次：`struct_gen` / `value_gen` / `recovery_epoch` / `list_gen`；
- 进程本地但只作提示、不产出可持久化数据的：`sbo_tls` 亲和、`fork_gen`、`store_id`。

**非法（不是"进程本地"，而是"把进程内地址当数据"）**：

1. `kvspaceXvalueBodyView` 返回 `head/body` 裸指针（`kvspace.h:108-116`、`kvspace.c:3122-3123`）；
2. `kvspaceViewToken_t.head/body` 裸指针（`kvspace.h:119-131`）；
3. 任何调用方缓存的 `head/body` 指针、或把指针写进 kvspace 的设计。

这三类在本进程内看似"零拷贝"，跨进程即指向任意内存；按用户裁决列为**非法设计，待删除**。

## 十一、待删除清单

| 技术 | 位置 | 为什么非法 | 删除方式 |
|---|---|---|---|
| `kvspaceXvalueBodyView` | `kvspace.h:108-116`、`kvspace.c:3122` | 返回映射内裸指针；跨进程基址不同，指针不可解释 | 新 ABI 只返回 `value_offset`；删除该函数与 `kvspaceXvalueBodyView_t` |
| `kvspaceViewToken_t.head/body` | `kvspace.h:119-131` | 同上；token 可跨进程传递时指针失效 | 保留 `struct_gen/value_gen/store_id/node_id`，删指针字段，用 `ref/offset` 重新解析 |
| 调用方指针缓存约定 | README / 各调用点 | "借用直到覆盖/删除/关闭"只是单进程内的生命周期，不能持久化 | 契约改为"只传偏移"；grep 调用点迁移 |

> 注：`sbo_tls`、`list_cached` 是进程本地实现，但前者只是分配亲和提示、后者用 shm 的 `list_gen` 做跨进程失效，**不列入删除**。判据是"跨进程可解释/失效可见"，不是"代码在进程内"。

## 十二、未采用 / 实验中的技术

- **50-cycle fastpath**：`kvspace-c/experiments/cycle-fastpath`（issue #57），未并入主代码；没有可引用的测量，不计入现状。
- 未发现批量提交、向量化、指令级并行等优化；当前写路径仍是单 key 单次提交。

## 十三、复现

```sh
# 覆盖写
cc -O3 -Isrc bench/overwrite.c -o /tmp/kvspace-overwrite -ldl
taskset -c 25 /tmp/kvspace-overwrite "$LIB" update-one 400000

# 直接子项枚举
python3 bench/compare_direct_children.py BASE CANDIDATE OUT --cpu 24 --pairs 9
```

依赖版本固定见 `kvspace-c/deps.json`；跨版本比较前先对齐编译器与 CPU。
