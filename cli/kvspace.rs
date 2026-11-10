// kvspace CLI — 走 libkvspace 的存储 C ABI。
//
// ## 只有 shm
//
// kvspace 是**单库**：shm 引擎直接编在 `libkvspace` 里，没有运行期 dispatch，也没有
// dlopen 任何后端 .so。`kvspaceConnect` 只接受 `shm://`，其它 scheme 一律返回 NULL。
//
// 「后端由 DSN 运行时选择」是**旧协议**，早已不存在——别再照着它写代码或文档：
// `src/` 里 dlopen/dlsym 命中 0，`kvspace_dsn_path` 只认 3 个字符的 `shm`。
//
// 为什么没有 dispatch：后端由**链接期**选择（链哪个 libkvspace 就是哪个后端），
// 不是运行期。运行期探测能力再降级，正是单库化要消灭的那条分支。
//
// ## 构建前提（缺一即链接失败）
//
// 链接期需要 `-lkvspace`：
//
//     cmake -S . -B build && cmake --build build -j          # 在本仓根
//     LIBRARY_PATH=<kvspace>/build cargo build --release      # ← 缺它会报
//                                                            #   unable to find library -lkvspace
//     LD_LIBRARY_PATH=<kvspace>/build ./target/release/kvspace help

use std::env;
use std::os::raw::{c_char, c_int, c_void};
use std::process::exit;

/* 固定 head 16 字节（见 kvspace/kvspace_xvalue.h）：[bodylen:u64@0][attrslen:u16@8]
   [padding:6@10-15]，body 偏移恒 16。bodylen 是容量还是有效长度由 langtype 判；
   指针/扩展性**不在 head**，看 langtype 首字符 `*`/`@`。 */
const KVSPACE_XVALUE_HEAD_LEN: usize = 16;
/* kvspace_status_t 的 MISSING（kvspace/kvspace.h:39，= -ENOENT）。只这一处需要判它。 */
const KVSPACE_MISSING: c_int = -2;

/* 每个 extern 后标注 `// N 参`：Rust 的 `extern "C"` **不校验参数个数**，
   与 C 头不符时会以 segfault（而非编译错误）出现——本文件就因 NewSlack 按旧五参
   写、新头是四参而崩过一次。标注让「对不上」在阅读时可见。 */
/* 只链 `kvspace`：本 CLI 只做存储子命令。语言层命令（get/set 等）在 kvlang 的 CLI
   里，那个才需要 libkvlang_xvalue。 */
#[link(name = "kvspace")]
extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void; // 1 参
    fn kvspaceClose(h: *mut c_void); // 1 参

    /* ── 新存储层 ABI（kvspace/kvspace.h，12 个符号） ─────────────────────
       旧 kvspaceGet/SetValue/ListLen/ListAt/Del/DelTree/CpTree/MkindexExt/
       Watch/Clear/New* 已全部删除：本文件不允许再出现它们的 extern。 */

    /* 进程内值区基址。本库（shm）恒非 NULL；返回 NULL 说明链到了一个非映射后端，
       CLI 见到即拒绝启动——那是链接期选错了库，不是运行期可降级的情形。 */
    fn kvspaceXvalueInProcessAddress(h: *mut c_void) -> *mut u8; // 1 参
    /* key → 偏移；anchor 0 = root。不跟 Ptr。 */
    /* 6 参 */
    fn kvspaceResolve(
        h: *mut c_void,
        key: *const c_char,
        parent_key_anchor: u64,
        out_value_offset: *mut u64,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    fn kvspaceFree(h: *mut c_void, key: *const c_char, err: *mut c_char, err_cap: u32) -> c_int; // 4 参
    /* 5 参 */
    fn kvspaceCp(
        h: *mut c_void,
        old_key: *const c_char,
        new_key: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    /* 四参、无 capacity。 */
    fn kvspaceMkindex(h: *mut c_void, key: *const c_char, err: *mut c_char, err_cap: u32) -> c_int; // 4 参（无 capacity）
    fn kvspaceDeltree(h: *mut c_void, key: *const c_char, err: *mut c_char, err_cap: u32) -> c_int; // 4 参
    /* 5 参 */
    fn kvspaceCpdir(
        h: *mut c_void,
        source_key: *const c_char,
        destination_key: *const c_char,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    /* 6 参 */
    fn kvspaceListlen(
        h: *mut c_void,
        key: *const c_char,
        out_count: *mut i64,
        out_max_name_length: *mut u32,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;
    /* 9 参 */
    fn kvspaceList(
        h: *mut c_void,
        key: *const c_char,
        out_names: *mut c_char,
        out_capacity: u32,
        out_length: *mut u32,
        out_count: *mut u32,
        out_generation: *mut u64,
        err: *mut c_char,
        err_cap: u32,
    ) -> c_int;

}

fn cs(s: &str) -> *const c_char {
    // &str 不保证 NUL 结尾（尤其字符串字面量），必须经 CString 补 NUL。
    // CLI 短生命周期，泄漏即可（进程退出统一回收）。
    std::ffi::CString::new(s).unwrap().into_raw() as *const c_char
}

/* 取 DSN：**没有默认值**。`KVSPACE` 未设置就直接失败。
 *
 * 两个被否掉的写法：
 *   - 默认 `redis://127.0.0.1:6379`：本库只认 `shm` scheme，于是「不设环境变量」
 *     这条最普通的新用户路径必然失败，而失败信息看不出是默认值本身的错；
 *   - 默认 `shm://./kvspace.shm`：能连上，但任何人不带变量敲一条命令就在当前目录
 *     静默落一个几十 MB 的存储文件。探针工具静默落文件比报错更坏——报错会让人去
 *     设变量，落文件会让人几周后才发现磁盘里多了几个。
 *
 * 判据与构建配置一致：不给隐式默认值，缺了就点名。 */
fn default_dsn() -> String {
    match env::var("KVSPACE") {
        Ok(value) if !value.is_empty() => value,
        _ => fatalf(
            "未设置 KVSPACE。本 CLI 只接受 shm://，例如：\n  \
             KVSPACE=shm:///tmp/kvspace.shm kvspace list /",
        ),
    }
}

fn fatalf(msg: &str) -> ! {
    eprintln!("{}", msg);
    exit(1)
}

// 后端可能成功也可能失败但不填 err（如 shm 若干写路径）：空 err 给兜底，绝不静默当成功。
fn fail_op(err: &[u8; 256], op: &str) -> ! {
    let n = err.iter().position(|&b| b == 0).unwrap_or(err.len());
    if n > 0 {
        fatalf(&String::from_utf8_lossy(&err[..n]));
    }
    fatalf(&format!("{}: failed（后端未填错误详情）", op));
}


/* head 固定 16 字节 [bodylen:u64@0][attrslen:u16@8][padding:6@10-15]，body 偏移恒 16，
   attr 区紧随 body 且无自带长度前缀（长度只在 head 里）；langtype 作为 attr 记录 tag=1
   置于 body 之后（值内，非独立 key）。值自描述：直接解 wire（取头函数已删除；它对 Ptr
   穿透，读不到本体）。head 里没有 layout / reserved / storetype / ptr。 */

/// bodylen:u64 小端 @0-7（唯一读点，与 C 侧 kvspaceXvalueHeadLen 一致）。

/// 扫 attr 区取 tag=1（langtype）；无记录/坏 attr → 空串。

/// langtype 无 ref 前缀（ref 是独立字段）：只剥前导 [dims]，其余为基 kind。

/// 是否 bufferarray：`char/<enc>` 基种类，或轴写成 `[]` / `[.]` 等运行期长度
/// （见 spec [[compact形态]]）。bufferarray 的 bodylen 是容量，其余是有效长度。


/// Element width in bytes for fixed-width scalars; None otherwise.

/// Decode one little-endian fixed-width scalar; returns None on unknown kind or short slice.




// 借用读：resolve_view 返回值区内的切片（共享映射），读出即拷贝自持，绝不 free。
/* ── 进程内值区（新 ABI 的唯一读写路径） ──────────────────────────────
   resolve 给偏移，InProcessAddress 给基址，地址 = base + offset。值区里的字节就是
   wire 本体：16 字节 head（Realloc/后端写）+ body + attr。

   写没有「commit」：Realloc 写 head 并把这段交给我们，填完 body/attr 函数返回即生效
   （shm 靠共享映射）。这是新 ABI 的真实语义，不发明提交点。 */

/// 该句柄的进程内值区基址。NULL = 链到了非映射后端（有值区才能读写）。
fn data_base(kv: *mut c_void) -> *mut u8 {
    unsafe { kvspaceXvalueInProcessAddress(kv) }
}

/// 读一个 key 的 wire 切片（借自值区，活到下一次写/删该 key）。
fn resolve_view<'a>(kv: *mut c_void, key: &str) -> Option<&'a [u8]> {
    let base = data_base(kv);
    if base.is_null() {
        return None;
    }
    let mut offset: u64 = 0;
    let mut err = [0u8; 256];
    let rc = unsafe {
        kvspaceResolve(
            kv,
            cs(key),
            0,
            &mut offset,
            err.as_mut_ptr() as *mut c_char,
            256,
        )
    };
    if rc != 0 {
        return None;
    }
    let head = unsafe { base.add(offset as usize) };
    /* bodylen@0-7、attrslen@8-9 决定整条 wire 的长度（头一处算出，不另算一份）。 */
    let raw = unsafe { std::slice::from_raw_parts(head, KVSPACE_XVALUE_HEAD_LEN) };
    let parsed = raw_head(raw)?;
    let total = usize::try_from(parsed.total).ok()?;
    Some(unsafe { std::slice::from_raw_parts(head, total) })
}

/// 拷贝读：给调用方一份独立字节（值区切片会在写时变化）。
/* 原始 head 字段：**不解释**。bodylen / attrslen 是字节数，padding 是 ABI 的一部分
   （非零即非法），total = 16 + bodylen + attrslen。这里不碰 langtype——那是语言层，
   `kvspace` 不认识「类型」这个概念。 */
struct RawHead {
    body_len: u64,
    attrs_len: u16,
    total: u64,
    padding_zero: bool,
}

fn raw_head(wire: &[u8]) -> Option<RawHead> {
    if wire.len() < KVSPACE_XVALUE_HEAD_LEN {
        return None;
    }
    let body_len = u64::from_le_bytes(wire[0..8].try_into().ok()?);
    let attrs_len = u16::from_le_bytes(wire[8..10].try_into().ok()?);
    let padding_zero = wire[10..16].iter().all(|b| *b == 0);
    let total = KVSPACE_XVALUE_HEAD_LEN as u64 + body_len + attrs_len as u64;
    Some(RawHead { body_len, attrs_len, total, padding_zero })
}

/* attr 区逐条拆 record：`[tag:u8][len:u16 LE][data len 字节]`，紧密排列、末尾不 pad。
   返回 `None` 表示「head 自洽但 attr 区装不下」——那是畸形，不猜。tag 只报编号，
   不解释含义（tag 1 是 langtype，存储层不需要知道）。 */
fn raw_attrs(wire: &[u8]) -> Option<Vec<(u8, Vec<u8>)>> {
    let head = raw_head(wire)?;
    let start = KVSPACE_XVALUE_HEAD_LEN + head.body_len as usize;
    let end = start + head.attrs_len as usize;
    if end > wire.len() {
        return None;
    }
    let mut records = Vec::new();
    let mut at = start;
    while at + 3 <= end {
        let tag = wire[at];
        let len = u16::from_le_bytes([wire[at + 1], wire[at + 2]]) as usize;
        if at + 3 + len > end {
            return None;
        }
        records.push((tag, wire[at + 3..at + 3 + len].to_vec()));
        at += 3 + len;
    }
    /* 尾部残余字节（不足一条 record）按畸形处理：它是「写读双方理解不一致」的证据，
       与 langtype 扫描器的语义一致。 */
    if at != end {
        return None;
    }
    Some(records)
}

fn hex(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len() * 2);
    for b in data {
        out.push_str(&format!("{:02x}", b));
    }
    out
}

fn get(kv: *mut c_void, key: &str) -> Vec<u8> {
    resolve_view(kv, key).map(|v| v.to_vec()).unwrap_or_default()
}

// Parse CLI value syntax into full XValue wire bytes (langtype stored in tag=1 attr).
// Empty Vec represents `nil:` (delete).

// codec 产出为 frontend malloc，读出即 free；失败已 fatalf 退出，到这里的 out 恒非空。

/* 写入一条完整 wire：head 由 `kvspaceRealloc` 写，本函数只把 body 与 attr 拷进值区。
   Realloc 的 body_length/attr_length 两个参数就是 head 的内容，所以不必也不该再写 head。 */

// Single-key full-value write (head + body + attr tail).

// Normalize directory prefix (trailing `/` or `·`): ListLen/ListAt, mkindex, and
// extindex mounts address directory keys; append `/` to bare prefixes like `/t`.
fn dir_form(prefix: &str) -> String {
    if prefix.ends_with('/') || prefix.ends_with('·') {
        prefix.to_string()
    } else {
        format!("{}/", prefix)
    }
}

/* 前缀列举：先用 Listlen 拿 (N, M)，再一次性 List 拿 NUL 分隔的名字。
   **顺序是后端顺序，不排序**——新 ABI 把排序上移给调用方，CLI 不再假装有序。 */
fn list_names(kv: *mut c_void, prefix: &str) -> Vec<String> {
    let p = cs(&dir_form(prefix));
    let mut count: i64 = 0;
    let mut max_len: u32 = 0;
    let mut err = [0u8; 256];
    let rc = unsafe {
        kvspaceListlen(
            kv,
            p,
            &mut count,
            &mut max_len,
            err.as_mut_ptr() as *mut c_char,
            256,
        )
    };
    if rc != 0 || count <= 0 {
        return Vec::new();
    }
    /* 容量按「N 个名字 + N 个 NUL」给：count*(max_len+1) 是上界，不必先探探测一次。 */
    let cap = (count as usize)
        .saturating_mul(max_len as usize + 1)
        .min(u32::MAX as usize) as u32;
    if cap == 0 {
        return Vec::new();
    }
    let mut buf = vec![0u8; cap as usize];
    let mut length: u32 = 0;
    let mut out_count: u32 = 0;
    let mut generation: u64 = 0;
    let rc = unsafe {
        kvspaceList(
            kv,
            p,
            buf.as_mut_ptr() as *mut c_char,
            cap,
            &mut length,
            &mut out_count,
            &mut generation,
            err.as_mut_ptr() as *mut c_char,
            256,
        )
    };
    if rc != 0 {
        fail_op(&err, "list");
    }
    buf.truncate(length as usize);
    buf.split(|b| *b == 0)
        .filter(|s| !s.is_empty())
        .map(|s| String::from_utf8_lossy(s).into_owned())
        .collect()
}

fn join_path(parent: &str, child: &str) -> String {
    if parent == "/" {
        return format!("/{}", child);
    }
    if parent.ends_with('/') || parent.ends_with('·') {
        return format!("{}{}", parent, child);
    }
    format!("{}/{}", parent, child)
}

/* 解析 `<prefix>`；默认 `/`。**没有 --kind**：kind 要从每条 wire 的 langtype 解出来，
   那是语言层的事，归 kvlang 的 CLI。 */
fn parse_walk_args(tail: &[String]) -> String {
    let mut prefix = "/".to_string();
    for a in tail {
        if a.starts_with("--") {
            fatalf(&format!("unknown option: {}", a));
        }
        prefix = a.clone();
    }
    prefix
}

fn print_help() {
    println!("usage: kvspace <subcommand> [args]   (存储地址由 env KVSPACE 给出，形如 shm:///path)");
    println!();
    println!("存储子命令（只处理 key 与原始字节，不解释值的内容）:");
    println!("  head <key>...           打印原始 head：bodylen/attrslen/total/padding");
    println!("  attr <key>...           逐条打印 attr 记录：tag/len/data(hex)");
    println!("  del <key>...            删除键");
    println!("  deltree <prefix>        删除整棵子树（含根）");
    println!("  cp <src> <dst>          拷贝单键");
    println!("  cpdir <src> <dst>       递归拷贝子树");
    println!("  mkindex <path>          创建目录（index）");
    println!("  list|ls <prefix>        列子键（后端顺序，不排序）");
    println!("  tree <prefix>           树状遍历（后端顺序，不排序）");
    println!("  clear                   清空 kvspace（等价 deltree /，不可恢复）");
    println!("  help                    print this help");
    println!();
    println!("读写值（get/set 与 kind 解释）在 kvlang 的 CLI：`kvlang get|set|list --kind`。");
    println!("本 CLI 不解析 langtype——那是语言层的事，存储层不认识「类型」。");
    println!();
    println!("存储地址: 只接受 shm://。其它 scheme 在 connect 时就返回 NULL、CLI 立即退出——");
    println!("          不是「连上之后才发现不支持」。没有运行期 dispatch，后端由链接期决定。");
    println!("          没有默认值：未设 KVSPACE 直接报错退出，不会替你落一个存储文件。");
}

fn main() {
    let rest: Vec<String> = env::args().skip(1).collect();
    let dsn = default_dsn();
    if rest.is_empty() {
        eprintln!("usage: kvspace <subcommand> [args]   (存储地址由 env KVSPACE 给出，只接受 shm://)");
        eprintln!("       kvspace help  查看子命令清单");
        exit(1);
    }
    let sub = rest[0].clone();
    if matches!(sub.as_str(), "help" | "--help" | "-h") {
        print_help();
        return; // help 不需要连接
    }

    let kv = unsafe { kvspaceConnect(cs(&dsn)) };
    if kv.is_null() {
        /* 只说「connect failed」等于没说：唯一可能的失败原因是 scheme 不是 shm
           （路径打不开时 kvspaceConnect 也返回 NULL，所以两种都要提到）。 */
        fatalf(&format!(
            "connect failed: {dsn}\n  \
             本 CLI 只接受 shm://（例如 shm:///tmp/kvspace.shm），其它 scheme 一律 NULL；\n  \
             若已是 shm://，检查该路径可写、父目录存在。"
        ));
    }

    let tail = &rest[1..];
    match sub.as_str() {
        "head" => {
            if tail.is_empty() {
                fatalf("usage: kvspace head <key>...");
            }
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                    continue;
                }
                match raw_head(&v) {
                    Some(h) => println!(
                        "{}\tbodylen={}\tattrslen={}\ttotal={}\tpadding={}",
                        k,
                        h.body_len,
                        h.attrs_len,
                        h.total,
                        if h.padding_zero { "zero" } else { "NONZERO" }
                    ),
                    None => println!("{}\t(short: {} bytes, < 16)", k, v.len()),
                }
            }
        }
        "attr" => {
            if tail.is_empty() {
                fatalf("usage: kvspace attr <key>...");
            }
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                    continue;
                }
                match raw_attrs(&v) {
                    Some(records) => {
                        if records.is_empty() {
                            println!("{}\t(no attr records)", k);
                        }
                        for (tag, data) in records {
                            println!("{}\ttag={}\tlen={}\tdata={}", k, tag, data.len(), hex(&data));
                        }
                    }
                    None => println!("{}\t(malformed: attr region does not fit)", k),
                }
            }
        }
        "del" => {
            if tail.is_empty() {
                fatalf("usage: kvspace del <key>...");
            }
            /* 逐个 Free：新 ABI 没有批量 Del。缺失的键跳过（与旧 Del 一致）。 */
            for k in tail {
                let mut err = [0u8; 256];
                let rc = unsafe { kvspaceFree(kv, cs(k), err.as_mut_ptr() as *mut c_char, 256) };
                if rc != 0 && rc != KVSPACE_MISSING {
                    fail_op(&err, &format!("del {}", k));
                }
            }
        }
        "deltree" => {
            let p = tail
                .first()
                .unwrap_or_else(|| fatalf("usage: kvspace deltree <prefix>"));
            let mut err = [0u8; 256];
            let rc = unsafe { kvspaceDeltree(kv, cs(p), err.as_mut_ptr() as *mut c_char, 256) };
            if rc != 0 {
                fail_op(&err, "deltree");
            }
        }
        "cp" => {
            if tail.len() < 2 {
                fatalf("usage: kvspace cp <src> <dst>");
            }
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceCp(
                    kv,
                    cs(&tail[0]),
                    cs(&tail[1]),
                    err.as_mut_ptr() as *mut c_char,
                    256,
                )
            };
            if rc != 0 {
                fail_op(&err, "cp");
            }
        }
        "cpdir" => {
            if tail.len() < 2 {
                fatalf("usage: kvspace cpdir <src> <dst>");
            }
            let mut err = [0u8; 256];
            let rc = unsafe {
                kvspaceCpdir(
                    kv,
                    cs(&tail[0]),
                    cs(&tail[1]),
                    err.as_mut_ptr() as *mut c_char,
                    256,
                )
            };
            if rc != 0 {
                fail_op(&err, "cpdir");
            }
        }
        "mkindex" => {
            let p = tail
                .first()
                .unwrap_or_else(|| fatalf("usage: kvspace mkindex <path>"));
            if tail.len() > 1 {
                fatalf("mkindex 不再接受 capacity（新 ABI 无该参数）");
            }
            // 目录形归一：CLI 统一补 /，三后端一致。
            let p = dir_form(p);
            let mut err = [0u8; 256];
            let rc = unsafe { kvspaceMkindex(kv, cs(&p), err.as_mut_ptr() as *mut c_char, 256) };
            if rc != 0 {
                fail_op(&err, "mkindex");
            }
        }
        "clear" => {
            /* 整库清空 = Deltree("/")。新 ABI 没有 Clear。 */
            eprintln!("warning: clear 删除整个 kvspace（等价 deltree /），不可恢复");
            let mut err = [0u8; 256];
            let rc = unsafe { kvspaceDeltree(kv, cs("/"), err.as_mut_ptr() as *mut c_char, 256) };
            if rc != 0 {
                fail_op(&err, "clear");
            }
        }
        "list" | "ls" => {
            let p = parse_walk_args(tail);
            fprint_list(kv, &p);
        }
        "tree" => {
            let p = parse_walk_args(tail);
            println!("{}", p);
            fprint_tree(kv, &p, "");
        }
        other => {
            eprintln!("unknown subcommand: {}", other);
            exit(1);
        }
    }

    unsafe {
        kvspaceClose(kv);
    }
}

/* 每行一子项。有值就打它的**原始 head 摘要**（bodylen/attrslen），不解释内容——
   把字节翻成 `kind:value` 是语言层的事。 */
fn fprint_list(kv: *mut c_void, prefix: &str) {
    /* 顺序 = 后端顺序，**不排序**：新 ABI 把排序上移，CLI 输出按 List 返回原序。 */
    for c in list_names(kv, prefix) {
        let full = join_path(prefix, &c);
        match raw_head(&get(kv, &full)) {
            Some(h) => println!("{}\tbodylen={}\tattrslen={}", c, h.body_len, h.attrs_len),
            None => println!("{}", c),
        }
    }
}

fn fprint_tree(kv: *mut c_void, prefix: &str, indent: &str) {
    /* 同样按后端顺序；树形分支线只表示「是不是最后一个」，不隐含排序语义。 */
    let children = list_names(kv, prefix);
    let n = children.len();
    for (i, c) in children.iter().enumerate() {
        let full = join_path(prefix, c);
        let last = i == n - 1;
        let branch = if last { "└── " } else { "├── " };
        let next_indent = format!("{}{}", indent, if last { "    " } else { "│   " });
        if c.ends_with('/') {
            println!("{}{}{}", indent, branch, c);
            fprint_tree(kv, &full, &next_indent);
        } else {
            match raw_head(&get(kv, &full)) {
                Some(h) => println!(
                    "{}{}{}\tbodylen={}\tattrslen={}",
                    indent, branch, c, h.body_len, h.attrs_len
                ),
                None => println!("{}{}{}", indent, branch, c),
            }
        }
    }
}
