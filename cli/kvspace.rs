// kvspace CLI — 走 libkvspace dispatch 前端（C ABI），后端由 DSN 运行时选择。
//
// ## 构建前提（缺一即链接失败）
//
// 链接期需要 `-lkvspace`，即 `kvspace` 仓的前端库：
//
//     cmake -S ../kvspace -B ../kvspace/build && cmake --build ../kvspace/build -j
//     LIBRARY_PATH=<kvspace>/build cargo build --release     # ← 缺它会报
//                                                          #   unable to find library -lkvspace
//     LD_LIBRARY_PATH=<kvspace>/build ./target/release/kvspace help
//
// 运行期后端由 DSN 选：`shm://` → libkvspace-c（映射后端），`fs://`/`redis://`/`s3://`
// → libkvspace_durable。前端在本库同目录及其 `kvspace/` 子目录找后端 .so。
//
// ## 只支持 shm
//
// `kvspaceXvalueInProcessAddress` 返回 NULL 的后端（fs/redis/s3）没有进程内值区，
// 读写无从落地，CLI 在写入入口直接报错退出，不退化成第二路径。

use std::env;
use std::os::raw::{c_char, c_int, c_void};
use std::process::exit;

/* 固定 head 16 字节（见 kvspace/kvspace_xvalue.h）：[bodylen:u64@0][attrslen:u16@8]
   [padding:6@10-15]，body 偏移恒 16。bodylen 是容量还是有效长度由 langtype 判；
   指针/扩展性**不在 head**，看 langtype 首字符 `*`/`@`。 */
const KVSPACE_XVALUE_HEAD_LEN: usize = 16;
const KVSPACE_XVALUE_PTR_PTR: u8 = 1;
const KVSPACE_XVALUE_PTR_EXT: u8 = 2;
/* kvspace_status_t 的 MISSING（kvspace/kvspace.h:39，= -ENOENT）。只这一处需要判它。 */
const KVSPACE_MISSING: c_int = -2;

/* 每个 extern 后标注 `// N 参`：Rust 的 `extern "C"` **不校验参数个数**，
   与 C 头不符时会以 segfault（而非编译错误）出现——本文件就因 NewSlack 按旧五参
   写、新头是四参而崩过一次。标注让「对不上」在阅读时可见。 */
#[link(name = "kvspace")]
extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void; // 1 参
    fn kvspaceClose(h: *mut c_void); // 1 参
    fn free(p: *mut c_void); // libc：释放构造器产出的 frontend malloc 缓冲

    /* ── 新存储层 ABI（kvspace/kvspace.h，12 个符号） ─────────────────────
       旧 kvspaceGet/SetValue/ListLen/ListAt/Del/DelTree/CpTree/MkindexExt/
       Watch/Clear/New* 已全部删除：本文件不允许再出现它们的 extern。 */

    /* 进程内值区基址；durable 的 fs/redis/s3 返回 NULL（CLI 见到即拒绝启动）。 */
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
    /* 唯一写原语：不存在则创建，存在则调整容量。head 由它写，调用方只填 body/attr。 */
    /* 7 参 */
    fn kvspaceRealloc(
        h: *mut c_void,
        key: *const c_char,
        body_length: u64,
        attr_length: u64,
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

    /* ── 值构造器（kvspace/kvlang_xvalue.h）：造完整 wire，CLI 拆 body/attr 后写 ── */
    /* 5 参 */
    fn kvspaceXvalueHeadNewScalar(
        raw: *const u8,
        raw_len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> c_int;
    /* 5 参 */
    fn kvspaceXvalueHeadNewSlack(
        data: *const u8,
        data_len: u64,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> c_int;
    /* 5 参 */
    fn kvspaceXvalueHeadNewPtr(
        path: *const u8,
        path_len: u32,
        langtype: *const c_char,
        out: *mut *mut u8,
        out_len: *mut u64,
    ) -> c_int;
}

fn cs(s: &str) -> *const c_char {
    // &str 不保证 NUL 结尾（尤其字符串字面量），必须经 CString 补 NUL。
    // CLI 短生命周期，泄漏即可（进程退出统一回收）。
    std::ffi::CString::new(s).unwrap().into_raw() as *const c_char
}

fn default_dsn() -> String {
    env::var("KVSPACE").unwrap_or_else(|_| "redis://127.0.0.1:6379".to_string())
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

struct Value {
    kind: String,   // Base kind (langtype without leading [dims])
    r#ref: u8,      // langtype 前缀：0=普通, 1=`*`, 2=`@`
    dims: Vec<i32>, // Leading [dims] parsed from langtype
    body: Vec<u8>,
    body_len: u64,  // head.bodylen
    attrs_len: u16, // head.attrslen
}

/* head 固定 16 字节 [bodylen:u64@0][attrslen:u16@8][padding:6@10-15]，body 偏移恒 16，
   attr 区紧随 body 且无自带长度前缀（长度只在 head 里）；langtype 作为 attr 记录 tag=1
   置于 body 之后（值内，非独立 key）。值自描述：直接解 wire（取头函数已删除；它对 Ptr
   穿透，读不到本体）。head 里没有 layout / reserved / storetype / ptr。 */
fn from_wire(data: &[u8]) -> Value {
    if data.len() < KVSPACE_XVALUE_HEAD_LEN {
        return Value {
            kind: String::new(),
            r#ref: 0,
            dims: vec![],
            body: vec![],
            body_len: 0,
            attrs_len: 0,
        };
    }
    let body_len = wire_len(data) as usize;
    /* 指针/扩展性由 langtype 前缀表达，不再是 head 的第 13 字节。 */
    let raw_langtype = wire_langtype(data, body_len);
    let (r#ref, langtype_body) = match raw_langtype.as_bytes().first() {
        Some(b'*') => (KVSPACE_XVALUE_PTR_PTR, &raw_langtype[1..]),
        Some(b'@') => (KVSPACE_XVALUE_PTR_EXT, &raw_langtype[1..]),
        _ => (0u8, &raw_langtype[..]),
    };
    let (dims, kind) = parse_langtype(langtype_body);
    /* body 只到 head.bodylen 为止；其后是 attr 区，不属 body。 */
    let start = KVSPACE_XVALUE_HEAD_LEN;
    let end = start.saturating_add(body_len).min(data.len());
    let attrs_len = (data[8] as u16) | ((data[9] as u16) << 8);
    Value {
        kind,
        r#ref,
        dims,
        body: data[start..end].to_vec(),
        body_len: body_len as u64,
        attrs_len,
    }
}

/// bodylen:u64 小端 @0-7（唯一读点，与 C 侧 kvspaceXvalueHeadLen 一致）。
fn wire_len(data: &[u8]) -> u64 {
    let mut v = 0u64;
    for i in (0..8).rev() {
        v = (v << 8) | data[i] as u64;
    }
    v
}

/// 扫 attr 区取 tag=1（langtype）；无记录/坏 attr → 空串。
fn wire_langtype(data: &[u8], body_len: usize) -> String {
    let base = KVSPACE_XVALUE_HEAD_LEN.saturating_add(body_len);
    /* attrslen:u16 @8-9；padding @10-15 六字节非零视为非法。attr 区无自带前缀。 */
    if data.len() < KVSPACE_XVALUE_HEAD_LEN || data[10..16].iter().any(|&b| b != 0) {
        return String::new();
    }
    let alen = (data[8] as usize) | ((data[9] as usize) << 8);
    if base + alen > data.len() {
        return String::new();
    }
    let end = base + alen;
    let mut p = base;
    while p + 3 <= end {
        let tag = data[p];
        let rlen = (data[p + 1] as usize) | ((data[p + 2] as usize) << 8);
        if p + 3 + rlen > end {
            return String::new();
        }
        if tag == 1 {
            return String::from_utf8_lossy(&data[p + 3..p + 3 + rlen]).into_owned();
        }
        p += 3 + rlen;
    }
    String::new()
}

/// langtype 无 ref 前缀（ref 是独立字段）：只剥前导 [dims]，其余为基 kind。
fn parse_langtype(lt: &str) -> (Vec<i32>, String) {
    if lt.contains('·') {
        return (Vec::new(), lt.to_string());
    }
    if let Some(rest) = lt.strip_prefix('[') {
        if let Some(end) = rest.find(']') {
            let dims: Vec<i32> = rest[..end]
                .split(',')
                .filter(|s| !s.is_empty())
                .map(|s| s.parse().unwrap_or(0))
                .collect();
            return (dims, rest[end + 1..].to_string());
        }
    }
    (Vec::new(), lt.to_string())
}

/// 是否 bufferarray：`char/<enc>` 基种类，或轴写成 `[]` / `[.]` 等运行期长度
/// （见 spec [[compact形态]]）。bufferarray 的 bodylen 是容量，其余是有效长度。

fn fmt_float(v: f64) -> String {
    let s = format!("{}", v);
    if s.contains('.') {
        s
    } else {
        format!("{}.0", s)
    }
}

/// Element width in bytes for fixed-width scalars; None otherwise.
fn scalar_width(kind: &str) -> Option<usize> {
    match kind {
        "bool" | "int8" | "uint8" => Some(1),
        "int16" | "uint16" => Some(2),
        "int32" | "uint32" | "float32" => Some(4),
        "int64" | "uint64" | "float64" => Some(8),
        _ => None,
    }
}

/// Decode one little-endian fixed-width scalar; returns None on unknown kind or short slice.
fn scalar_text(kind: &str, b: &[u8]) -> Option<String> {
    let w = scalar_width(kind)?;
    let b = b.get(..w)?;
    Some(match kind {
        "bool" => (b[0] != 0).to_string(),
        "int8" => (b[0] as i8).to_string(),
        "uint8" => b[0].to_string(),
        "int16" => i16::from_le_bytes(b.try_into().ok()?).to_string(),
        "uint16" => u16::from_le_bytes(b.try_into().ok()?).to_string(),
        "int32" => i32::from_le_bytes(b.try_into().ok()?).to_string(),
        "uint32" => u32::from_le_bytes(b.try_into().ok()?).to_string(),
        "float32" => fmt_float(f32::from_le_bytes(b.try_into().ok()?) as f64),
        "int64" => i64::from_le_bytes(b.try_into().ok()?).to_string(),
        "uint64" => u64::from_le_bytes(b.try_into().ok()?).to_string(),
        "float64" => fmt_float(f64::from_le_bytes(b.try_into().ok()?)),
        _ => return None,
    })
}

fn join_dims(dims: &[i32]) -> String {
    dims.iter()
        .map(|d| d.to_string())
        .collect::<Vec<_>>()
        .join(",")
}

fn plain(v: &Value) -> String {
    // ref 先于 kind 判空：不约束指针（空 kindexpr）也是合法值。
    if v.r#ref == KVSPACE_XVALUE_PTR_PTR {
        return format!("→{}", String::from_utf8_lossy(&v.body));
    }
    if v.r#ref == KVSPACE_XVALUE_PTR_EXT {
        return format!("@{}", String::from_utf8_lossy(&v.body));
    }
    if v.kind.is_empty() {
        return "None".to_string();
    }
    if let Some(w) = scalar_width(&v.kind) {
        // Format fixed-width numeric tensors ([dims]) element-by-element.
        if !v.dims.is_empty() {
            let items: Vec<String> = v
                .body
                .chunks_exact(w)
                .filter_map(|c| scalar_text(&v.kind, c))
                .collect();
            return format!("[{}]", items.join(","));
        }
        return scalar_text(&v.kind, &v.body)
            .unwrap_or_else(|| format!("<short body: {} bytes>", v.body.len()));
    }
    match v.kind.as_str() {
        "char/utf32" => v
            .body
            .chunks_exact(4)
            .map(|c| {
                char::from_u32(u32::from_le_bytes([c[0], c[1], c[2], c[3]])).unwrap_or('\u{FFFD}')
            })
            .collect(),
        "stringkeymap" => format!("map[{}]", join_dims(&v.dims)),
        "index" => format!("({})", v.dims.first().copied().unwrap_or(0)),
        // char/utf8, char/ascii, and other kinds: decode as lossy UTF-8.
        _ => String::from_utf8_lossy(&v.body).into_owned(),
    }
}

fn format_value(v: &Value) -> String {
    if v.r#ref == KVSPACE_XVALUE_PTR_PTR {
        return format!("→{}:{}", String::from_utf8_lossy(&v.body), v.kind);
    }
    if v.kind.is_empty() {
        return plain(v); // None or @ext locator
    }
    format!("{}:{}", v.kind, plain(v))
}

// 借用读：resolve_view 返回值区内的切片（共享映射），读出即拷贝自持，绝不 free。
/* ── 进程内值区（新 ABI 的唯一读写路径） ──────────────────────────────
   resolve 给偏移，InProcessAddress 给基址，地址 = base + offset。值区里的字节就是
   wire 本体：16 字节 head（Realloc/后端写）+ body + attr。

   写没有「commit」：Realloc 写 head 并把这段交给我们，填完 body/attr 函数返回即生效
   （shm 靠共享映射）。这是新 ABI 的真实语义，不发明提交点。 */

/// 该句柄的进程内值区基址。NULL = 后端无映射区（fs/redis/s3）→ CLI 不支持读写。
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
    /* bodylen@0-7、attrslen@8-9 决定整条 wire 的长度。 */
    let raw = unsafe { std::slice::from_raw_parts(head, KVSPACE_XVALUE_HEAD_LEN) };
    let body_len = wire_len(raw) as usize;
    let attrs_len = (raw[8] as usize) | ((raw[9] as usize) << 8);
    let total = KVSPACE_XVALUE_HEAD_LEN
        .checked_add(body_len)?
        .checked_add(attrs_len)?;
    Some(unsafe { std::slice::from_raw_parts(head, total) })
}

/// 拷贝读：给调用方一份独立字节（值区切片会在写时变化）。
fn get(kv: *mut c_void, key: &str) -> Vec<u8> {
    resolve_view(kv, key).map(|v| v.to_vec()).unwrap_or_default()
}

// Parse CLI value syntax into full XValue wire bytes (langtype stored in tag=1 attr).
// Empty Vec represents `nil:` (delete).
fn parse_value(raw: &str) -> Vec<u8> {
    let mut out: *mut u8 = std::ptr::null_mut();
    let len: u32;
    if let Some(rest) = raw.strip_prefix('*') {
        // `*:kindexpr:target` (typed ptr) or `*:target` (unconstrained ptr).
        let rest = rest.strip_prefix(':').unwrap_or(rest);
        let (k, t) = match rest.find(':') {
            Some(i) => (&rest[..i], &rest[i + 1..]),
            None => ("", rest),
        };
        let mut len64: u64 = 0;
        if unsafe {
            kvspaceXvalueHeadNewPtr(
                t.as_ptr(),
                t.len() as u32,
                cs(k),
                &mut out,
                &mut len64,
            ) != 0
        } {
            fatalf(&format!("parse value failed: {:?}", raw));
        }
        return take_wire(out, len64 as u32);
    }
    if let Some(langtype) = raw.strip_prefix("map:") {
        if !langtype.contains('·') {
            fatalf("map requires key·value langtype");
        }
        // Empty-body short header with langtype in the attr region.
        let mut len64: u64 = 0;
        if unsafe {
            kvspaceXvalueHeadNewScalar(
                std::ptr::null(),
                0,
                cs(langtype),
                &mut out,
                &mut len64,
            ) != 0
        } {
            fatalf(&format!("parse value failed: {:?}", raw));
        }
        return take_wire(out, len64 as u32); // bare kindexpr (e.g. int·string)
    }
    let split = raw.find(':').unwrap_or(raw.len());
    let kind = &raw[..split];
    let repr = raw[split..].trim_start_matches(':');
    let langtype = match kind {
        "int" => "int64",
        "float" => "float64",
        "float32" => "float32",
        "bool" => "bool",
        _ => "[]char/utf8", // string 与默认字符串
    };
    match kind {
        "int" => {
            let n: i64 = repr
                .parse()
                .unwrap_or_else(|_| fatalf(&format!("parse value failed: {:?}", raw)));
            let le = n.to_le_bytes();
            let mut len64: u64 = 0;
            unsafe {
                if kvspaceXvalueHeadNewScalar(
                    le.as_ptr(),
                    8,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
                {
                    fatalf(&format!("parse value failed: {:?}", raw));
                }
            }
            len = len64 as u32;
        }
        "float" => {
            let f: f64 = repr
                .parse()
                .unwrap_or_else(|_| fatalf(&format!("parse value failed: {:?}", raw)));
            let le = f.to_le_bytes();
            let mut len64: u64 = 0;
            unsafe {
                if kvspaceXvalueHeadNewScalar(
                    le.as_ptr(),
                    8,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
                {
                    fatalf(&format!("parse value failed: {:?}", raw));
                }
            }
            len = len64 as u32;
        }
        "float32" => {
            let f: f32 = repr
                .parse()
                .unwrap_or_else(|_| fatalf(&format!("parse value failed: {:?}", raw)));
            // 4 字节小端 f32 body 的短定长头。
            let le = f.to_le_bytes();
            let mut len64: u64 = 0;
            if unsafe {
                kvspaceXvalueHeadNewScalar(
                    le.as_ptr(),
                    4,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
            } {
                fatalf(&format!("parse value failed: {:?}", raw));
            }
            len = len64 as u32;
        }
        "bool" => {
            let b = match repr {
                "true" => true,
                "false" => false,
                _ => fatalf(&format!("bool 只接受 true/false: {:?}", raw)),
            };
            let body = [b as u8];
            let mut len64: u64 = 0;
            unsafe {
                if kvspaceXvalueHeadNewScalar(
                    body.as_ptr(),
                    1,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
                {
                    fatalf(&format!("parse value failed: {:?}", raw));
                }
            }
            len = len64 as u32;
        }
        "string" => {
            let mut len64: u64 = 0;
            unsafe {
                if kvspaceXvalueHeadNewSlack(
                    repr.as_ptr(),
                    repr.len() as u64,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
                {
                    fatalf(&format!("parse value failed: {:?}", raw));
                }
            }
            len = len64 as u32;
        }
        "nil" => return Vec::new(), // delete (no value)
        "index" => fatalf("use mkindex for directories"),
        // Default to string when no recognized kind prefix is present.
        _ => {
            let mut len64: u64 = 0;
            unsafe {
                if kvspaceXvalueHeadNewSlack(
                    raw.as_ptr(),
                    raw.len() as u64,
                    cs(langtype),
                    &mut out,
                    &mut len64,
                ) != 0
                {
                    fatalf(&format!("parse value failed: {:?}", raw));
                }
            }
            len = len64 as u32;
        }
    }
    take_wire(out, len)
}

// codec 产出为 frontend malloc，读出即 free；失败已 fatalf 退出，到这里的 out 恒非空。
fn take_wire(out: *mut u8, len: u32) -> Vec<u8> {
    unsafe {
        let v = std::slice::from_raw_parts(out, len as usize).to_vec();
        free(out as *mut c_void); // codec 产出为 frontend malloc，libc free
        v
    }
}

/* 写入一条完整 wire：head 由 `kvspaceRealloc` 写，本函数只把 body 与 attr 拷进值区。
   Realloc 的 body_length/attr_length 两个参数就是 head 的内容，所以不必也不该再写 head。 */
fn write_wire(kv: *mut c_void, key: &str, wire: &[u8]) {
    let base = data_base(kv);
    if base.is_null() {
        fatalf("本后端无进程内值区（fs/redis/s3），CLI 读写不支持");
    }
    if wire.len() < KVSPACE_XVALUE_HEAD_LEN {
        fatalf(&format!("set {}: 构造器产出的 wire 短于 head", key));
    }
    let body_len = wire_len(wire) as usize;
    let attrs_len = (wire[8] as usize) | ((wire[9] as usize) << 8);
    let body_at = KVSPACE_XVALUE_HEAD_LEN;
    let attr_at = body_at + body_len;
    if attr_at + attrs_len > wire.len() {
        fatalf(&format!("set {}: wire 自述长度越界", key));
    }

    let mut offset: u64 = 0;
    let mut err = [0u8; 256];
    let rc = unsafe {
        kvspaceRealloc(
            kv,
            cs(key),
            body_len as u64,
            attrs_len as u64,
            &mut offset,
            err.as_mut_ptr() as *mut c_char,
            256,
        )
    };
    if rc != 0 {
        fail_op(&err, &format!("set {}", key));
    }

    /* 落到 base+offset：body 在 +16，attr 紧随 body。head 已由 Realloc 写好。 */
    unsafe {
        let dst = base.add(offset as usize);
        if body_len > 0 {
            std::ptr::copy_nonoverlapping(wire[body_at..].as_ptr(), dst.add(body_at), body_len);
        }
        if attrs_len > 0 {
            std::ptr::copy_nonoverlapping(wire[attr_at..].as_ptr(), dst.add(attr_at), attrs_len);
        }
    }
    /* offset 到此作废：不跨调用持有，下次可能被搬移。 */
}

// Single-key full-value write (head + body + attr tail).
fn set_one(kv: *mut c_void, key: &str, val: &[u8]) {
    if val.is_empty() {
        // `nil:` deletes the key; missing key is not an error for `free`? 旧 Del 对缺失返回 0,
        // 新 Free 返回 MISSING —— 与旧行为一致：不存在就什么都不做，不报错。
        let mut err = [0u8; 256];
        let rc = unsafe { kvspaceFree(kv, cs(key), err.as_mut_ptr() as *mut c_char, 256) };
        /* KVSPACE_MISSING = -ENOENT：删不存在的键不算错（与旧 Del 一致）。 */
        if rc != 0 && rc != KVSPACE_MISSING {
            fail_op(&err, &format!("set {} nil", key));
        }
        return;
    }
    write_wire(kv, key, val);
}

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

// Shared list/tree arguments: [prefix] [--kind[=false]]; default prefix is `/`.
fn parse_walk_args(tail: &[String], show_kind_default: bool) -> (String, bool) {
    let mut show_kind = show_kind_default;
    let mut prefix = "/".to_string();
    for a in tail {
        match a.as_str() {
            "--kind" => show_kind = true,
            "--kind=false" => show_kind = false,
            other if other.starts_with("--showext") => {
                fatalf("--showext 已移除：新 ABI 无 extindex")
            }
            _ => prefix = a.clone(),
        }
    }
    (prefix, show_kind)
}

fn print_help() {
    println!("usage: kvspace <subcommand> [args]   (后端由 env KVSPACE 的 DSN 选择)");
    println!();
    println!("子命令:");
    println!("  get <key>...            读取并打印 key:kind:value");
    println!("  set <key> <value>       写入值；value 语法见下；nil: 删除键");
    println!("  head <key>...           打印值头（ref/ndim/dims/bodylen/attrslen）");
    println!("  del <key>...            删除键");
    println!("  deltree <prefix>        删除整棵子树（含根）");
    println!("  cp <src> <dst>          拷贝单键");
    println!("  cpdir <src> <dst>       递归拷贝子树");
    println!("  mkindex <path>          创建目录（index）；无 capacity 参数");
    println!("  list|ls <prefix> [--kind[=false]]  列子键（后端顺序，不排序）");
    println!("  tree <prefix> [--kind]  树状遍历（后端顺序，不排序）");
    println!("  clear                   清空 kvspace（等价 deltree /，不可恢复）");
    println!("  help                    print this help");
    println!();
    println!("后端限制: 仅 shm:// 支持读写（进程内值区）；fs/redis/s3 无映射区，connect 后即拒绝。");
    println!();
    println!("value 语法: [kind:]payload");
    println!("  int:42  float:3.14  float32:1.5  bool:true|false  string:foo");
    println!("  map:key·value  空 map（如 map:int·string）");
    println!("  *:kindexpr:target  指针（如 *:int64:/x）；*:target 为不约束指针");
    println!("  无 kind 前缀或未识别前缀 → 默认字符串");
}

fn main() {
    let rest: Vec<String> = env::args().skip(1).collect();
    let dsn = default_dsn();
    if rest.is_empty() {
        eprintln!("usage: kvspace <subcommand> [args]   (后端由 env KVSPACE 的 DSN 选择)");
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
        fatalf(&format!("connect failed: {dsn}"));
    }

    let tail = &rest[1..];
    match sub.as_str() {
        "get" => {
            if tail.is_empty() {
                fatalf("usage: kvspace get <key>...");
            }
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                } else {
                    println!("{}\t{}", k, format_value(&from_wire(&v)));
                }
            }
        }
        "set" => {
            if tail.len() != 2 {
                fatalf("usage: kvspace set <key> <value>");
            }
            set_one(kv, &tail[0], &parse_value(&tail[1]));
        }
        "head" => {
            if tail.is_empty() {
                fatalf("usage: kvspace head <key>...");
            }
            for k in tail {
                let v = get(kv, k);
                if v.is_empty() {
                    println!("{}\t(nil)", k);
                } else {
                    // ndim/dims come from leading [dims] in langtype.
                    let d = from_wire(&v);
                    println!(
                        "{}\t{}\tref={}\tndim={}\tdims=[{}]\tbodylen={}\tattrslen={}",
                        k,
                        d.kind,
                        d.r#ref,
                        d.dims.len(),
                        join_dims(&d.dims),
                        d.body_len,
                        d.attrs_len
                    );
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
            let (p, show_kind) = parse_walk_args(tail, true);
            fprint_list(kv, &p, show_kind);
        }
        "tree" => {
            let (p, show_kind) = parse_walk_args(tail, false);
            println!("{}", p);
            fprint_tree(kv, &p, "", show_kind);
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

fn fprint_list(kv: *mut c_void, prefix: &str, show_kind: bool) {
    /* 顺序 = 后端顺序，**不排序**：新 ABI 把排序上移，CLI 输出按 List 返回原序。 */
    for c in list_names(kv, prefix) {
        let v = get(kv, &join_path(prefix, &c));
        if v.is_empty() {
            println!("{}", c);
            continue;
        }
        let d = from_wire(&v);
        if show_kind {
            println!("{}\t{}\t{}", c, d.kind, plain(&d));
        } else {
            println!("{}\t{}", c, plain(&d));
        }
    }
}

fn fprint_tree(kv: *mut c_void, prefix: &str, indent: &str, show_kind: bool) {
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
            fprint_tree(kv, &full, &next_indent, show_kind);
        } else {
            let v = get(kv, &full);
            if v.is_empty() {
                println!("{}{}{}", indent, branch, c);
                continue;
            }
            let d = from_wire(&v);
            if show_kind {
                println!("{}{}{}\t{}\t{}", indent, branch, c, d.kind, plain(&d));
            } else {
                println!("{}{}{}\t{}", indent, branch, c, plain(&d));
            }
        }
    }
}
