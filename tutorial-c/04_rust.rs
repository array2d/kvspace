// 04_rust — Rust FFI 直调 libkvspace（dispatch 前端），shm:// 后端
// head 固定 16 字节 [bodylen:u64@0][attrslen:u16@8][padding:6@10-15]，body 偏移恒 16，
// langtype 在 body 之后的 attr 区。bodylen 恒为有效字节数，容量归存储后端。
// 编译: rustc 04_rust.rs -l kvspace

use std::convert::TryInto;
use std::ffi::{c_char, c_int, c_void, CString};
use std::ptr;

const KVSPACE_XVALUE_HEAD_LEN: usize = 16;
const KVSPACE_XVALUE_ATTR_TAG_LANGTYPE: u8 = 1;

/// 扫描 16 字节 head + attr 区，返回 langtype（无记录/坏 wire → 空）。
/// 与 C 的 `kvspaceXvalueHeadScan` 同语义；Rust 侧没有 inline 头可用，
/// 这里按线格式直接解，**不复制 C 的结构体字段**（那是第二份字段真相）。
/// wire: [bodylen:u64][attrslen:u16][padding:6][body][attrs]
fn xvalue_head_scan(wire: &[u8]) -> Vec<u8> {
    if wire.len() < KVSPACE_XVALUE_HEAD_LEN {
        return vec![];
    }
    let bodylen = u64::from_le_bytes(wire[0..8].try_into().unwrap()) as usize;
    let attrslen = u16::from_le_bytes(wire[8..10].try_into().unwrap()) as usize;
    if wire[10..16] != [0u8; 6] {
        return vec![];
    }
    if KVSPACE_XVALUE_HEAD_LEN + bodylen + attrslen > wire.len() {
        return vec![];
    }
    let mut p = KVSPACE_XVALUE_HEAD_LEN + bodylen;
    let mut left = attrslen;
    let mut langtype = vec![];
    while left >= 3 {
        let tag = wire[p];
        let rlen = u16::from_le_bytes([wire[p + 1], wire[p + 2]]) as usize;
        if left < 3 + rlen {
            return vec![];
        }
        match tag {
            KVSPACE_XVALUE_ATTR_TAG_LANGTYPE => langtype = wire[p + 3..p + 3 + rlen].to_vec(),
            2 => return vec![], // 旧 reallen tag 已删除，出现即拒
            _ => {}
        }
        p += 3 + rlen;
        left -= 3 + rlen;
    }
    langtype
}

/// 有效长度 = bodylen（head 里没有 layout，也没有 tag 2）。
fn xvalue_head_effective_len(wire: &[u8]) -> Option<u64> {
    if wire.len() < KVSPACE_XVALUE_HEAD_LEN {
        return None;
    }
    Some(u64::from_le_bytes(wire[0..8].try_into().unwrap()))
}

extern "C" {
    fn kvspaceConnect(dsn: *const c_char) -> *mut c_void;
    fn kvspaceClose(h: *mut c_void);
    // codec 产出为 frontend malloc 缓冲，拷出后以 libc free 释放（无 kvspaceBytesFree）。
    fn free(p: *mut c_void);
    // 借用读：out 指向 kvspace 常驻空间，调用方不得 free。resolve=1 穿透 link。
    fn kvspaceGet(h: *mut c_void, key: *const c_char, resolve: c_int,
                  out: *mut *mut u8, out_len: *mut u32) -> c_int;
    // 就地写：key 已存在且 body_len 不变 → 返回原 box body 偏移指针；否则非 0。
    fn kvspaceWriteInPlace(h: *mut c_void, key: *const c_char, resolve: c_int, body_len: u32,
                           body: *mut *mut u8, err: *mut c_char, err_cap: u32) -> c_int;
    // Allocate box and write fixed header + attr region (including langtype), returning body pointer.
    fn kvspaceWriteNewPlace(h: *mut c_void, key: *const c_char, r#ref: u8, storetype: u8,
                            body_len: u32, body_cap: u64, langtype: *const c_char,
                            body: *mut *mut u8, err: *mut c_char, err_cap: u32) -> c_int;
    // 前缀枚举：ListLen 定计数，逐 idx ListAt 借用取名（不得 free）。
    fn kvspaceListLen(h: *mut c_void, prefix: *const c_char, expand_ext: c_int,
                      resolve: c_int, out_count: *mut i32) -> c_int;
    fn kvspaceListAt(h: *mut c_void, prefix: *const c_char, expand_ext: c_int, resolve: c_int,
                     idx: i32, out: *mut u8, out_cap: u32, out_len: *mut u32) -> c_int;
    fn kvspaceDel(h: *mut c_void, keys: *const *const c_char, n: u32, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceDelTree(h: *mut c_void, prefix: *const c_char, err: *mut c_char, err_cap: u32) -> c_int;
    fn kvspaceMkindex(h: *mut c_void, path: *const c_char, capacity: u32, err: *mut c_char, err_cap: u32) -> c_int;
    // New*: langtype is stored in inline attr (empty = untyped).
    fn kvspaceNewInt64(v: i64, langtype: *const c_char, out: *mut *mut u8, out_len: *mut u32) -> c_int;
    fn kvspaceNewChar(bytes: *const u8, len: u32, langtype: *const c_char,
                      out: *mut *mut u8, out_len: *mut u32) -> c_int;
}

fn cs(s: &str) -> CString { CString::new(s).unwrap() }

fn enc_int(v: i64) -> Vec<u8> {
    let mut out: *mut u8 = ptr::null_mut();
    let mut n: u32 = 0;
    let lt = cs("int64");
    assert_eq!(unsafe { kvspaceNewInt64(v, lt.as_ptr(), &mut out, &mut n) }, 0);
    let b = unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec();
    unsafe { free(out as *mut c_void); }
    b
}

fn new_str(s: &str) -> Vec<u8> {
    let b = s.as_bytes();
    let mut out: *mut u8 = ptr::null_mut();
    let mut n: u32 = 0;
    let lt = cs("char/utf8");
    assert_eq!(unsafe { kvspaceNewChar(b.as_ptr(), b.len() as u32, lt.as_ptr(), &mut out, &mut n) }, 0);
    let r = unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec();
    unsafe { free(out as *mut c_void); }
    r
}

// kind 与 body 长度都从**同一条 wire**解出（kvspaceGet 的返回值），
// 不再调已删除的 kvspaceGetHead，也不维护第二份 head 结构体。
fn read_kind(_kv: *mut c_void, _key: &str, data: &[u8]) -> (String, Vec<u8>) {
    let langtype = xvalue_head_scan(data);
    let klen = langtype.iter().position(|&b| b == 0).unwrap_or(langtype.len());
    let mut kx = String::from_utf8_lossy(&langtype[..klen]).into_owned();
    if kx.starts_with('*') || kx.starts_with('@') { kx.drain(..1); }
    if let Some(i) = kx.find(']') { kx.drain(..=i); }
    let bl = xvalue_head_effective_len(data).unwrap_or(0) as usize;
    let end = (KVSPACE_XVALUE_HEAD_LEN + bl).min(data.len());
    (kx, data[KVSPACE_XVALUE_HEAD_LEN..end].to_vec())
}

struct KV { ptr: *mut c_void, path: String }

impl KV {
    fn open(path: &str) -> Self {
        let _ = std::fs::remove_file(path);
        let dsn = CString::new(format!("shm://{}", path)).unwrap();
        let ptr = unsafe { kvspaceConnect(dsn.as_ptr()) };
        assert!(!ptr.is_null());
        KV { ptr, path: path.to_string() }
    }
    // Write into returned body pointer; WriteNewPlace writes langtype with the header/attr region.
    fn set(&self, key: &str, langtype: &str, val: &[u8]) {
        if val.len() < KVSPACE_XVALUE_HEAD_LEN { return; }
        let bl = xvalue_head_effective_len(val).expect("value head is not well-formed") as u32;
        // ref 由 langtype 的 * / @ 前缀隐含，不再从 head 位取。
        let r#ref: u8 = if langtype.starts_with('*') { 1 }
                        else if langtype.starts_with('@') { 2 } else { 0 };
        let ck = cs(key);
        let lt = cs(langtype);
        let mut dst: *mut u8 = ptr::null_mut();
        let mut err = [0i8; 256];
        unsafe {
            if kvspaceWriteInPlace(self.ptr, ck.as_ptr(), 1, bl, &mut dst, err.as_mut_ptr(), 256) != 0 {
                let rc = kvspaceWriteNewPlace(self.ptr, ck.as_ptr(), r#ref, 0,
                                              bl, bl as u64, lt.as_ptr(), &mut dst,
                                              err.as_mut_ptr(), 256);
                assert_eq!(rc, 0, "write {key} failed");
            }
            if bl > 0 && !dst.is_null() {
                ptr::copy_nonoverlapping(
                    val[KVSPACE_XVALUE_HEAD_LEN..].as_ptr(), dst, bl as usize);
            }
        }
    }
    // 借用读：out 指向 kvspace 常驻空间，拷出自持后即失效，不 free。
    fn get(&self, key: &str) -> Option<Vec<u8>> {
        let ck = CString::new(key).unwrap();
        let mut out: *mut u8 = ptr::null_mut();
        let mut n: u32 = 0;
        unsafe { kvspaceGet(self.ptr, ck.as_ptr(), 0, &mut out, &mut n); }
        if out.is_null() || n == 0 { return None; }
        Some(unsafe { std::slice::from_raw_parts(out, n as usize) }.to_vec())
    }
    fn mkindex(&self, path: &str) {
        let cp = CString::new(path).unwrap();
        unsafe { kvspaceMkindex(self.ptr, cp.as_ptr(), 0, ptr::null_mut(), 0); }
    }
    fn del(&self, key: &str) {
        let ck = CString::new(key).unwrap();
        let keys = [ck.as_ptr()];
        unsafe { kvspaceDel(self.ptr, keys.as_ptr(), 1, ptr::null_mut(), 0); }
    }
    fn deltree(&self, prefix: &str) {
        let cp = CString::new(prefix).unwrap();
        unsafe { kvspaceDelTree(self.ptr, cp.as_ptr(), ptr::null_mut(), 0); }
    }
    // 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名（读出即拷入 Vec）。
    fn list(&self, prefix: &str) -> Vec<String> {
        let cp = CString::new(prefix).unwrap();
        let mut count: i32 = 0;
        if unsafe { kvspaceListLen(self.ptr, cp.as_ptr(), 0, 1, &mut count) } != 0 || count <= 0 {
            return vec![];
        }
        let mut r = Vec::with_capacity(count as usize);
        for i in 0..count {
            let mut nm = [0u8; 1024];
            let mut n: u32 = 0;
            unsafe { kvspaceListAt(self.ptr, cp.as_ptr(), 0, 1, i, nm.as_mut_ptr(), nm.len() as u32, &mut n); }
            if n > 0 {
                r.push(String::from_utf8_lossy(&nm[..n as usize]).into_owned());
            }
        }
        r
    }
}

impl Drop for KV {
    fn drop(&mut self) {
        unsafe { kvspaceClose(self.ptr); }
        let _ = std::fs::remove_file(&self.path);
    }
}

fn main() {
    let kv = KV::open("/tmp/kvspace_rs.shm");
    kv.mkindex("/rs/");
    kv.set("/rs/a", "int64", &enc_int(42));
    kv.set("/rs/b", "char/utf8", &new_str("hello"));

    let v = kv.get("/rs/a").unwrap();
    let (kind, raw) = read_kind(kv.ptr, "/rs/a", &v);
    let mut arr = [0u8; 8];
    arr.copy_from_slice(&raw[..8]);
    let val = i64::from_le_bytes(arr);
    println!("/rs/a  kind={kind}  val={val}");

    let ns = kv.list("/rs/");
    println!("list /rs/: {ns:?} (count={})", ns.len());

    kv.deltree("/rs/");
    println!("PASS rust");
}
