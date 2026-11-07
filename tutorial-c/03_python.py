#!/usr/bin/env python3
"""03_python — ctypes 直调 libkvspace（dispatch 前端），shm:// 后端
#571: Fixed 8-byte header (body offset is always 8); langtype in inline attr region."""

import ctypes
import os
import struct

SO = os.environ.get("KVSPACE_SO", "libkvspace.so")
lib = ctypes.CDLL(SO)

U8P = ctypes.POINTER(ctypes.c_uint8)
U32P = ctypes.POINTER(ctypes.c_uint32)
U64P = ctypes.POINTER(ctypes.c_uint64)
CCHARPP = ctypes.POINTER(ctypes.c_char_p)

KVSPACE_XVALUE_HEAD_LEN = 16
KVSPACE_XVALUE_LAYOUT_CAPACITY = 1
KVSPACE_XVALUE_ATTR_TAG_LANGTYPE = 1
KVSPACE_XVALUE_ATTR_TAG_REALLEN = 2


def xvalue_head_scan(wire):
    """扫描 16 字节 head + attr 区，返回 (langtype_bytes, reallen)。

    与 C 的 `kvspaceXvalueHeadScan` 同语义；Python 侧没有 inline 头可用，
    这里按线格式直接解，**不复制 C 的结构体字段**（那是第二份字段真相）。
    wire: [bodylen:u64][attrslen:u32][layout:u8][padding:3][body][attrs]
    """
    if len(wire) < KVSPACE_XVALUE_HEAD_LEN:
        return b"", None
    bodylen = int.from_bytes(wire[0:8], "little")
    attrslen = int.from_bytes(wire[8:12], "little")
    if wire[13:16] != b"\0\0\0":
        return b"", None
    if KVSPACE_XVALUE_HEAD_LEN + bodylen + attrslen > len(wire):
        return b"", None
    base = KVSPACE_XVALUE_HEAD_LEN + bodylen
    langtype, reallen, p, left = b"", None, base, attrslen
    while left >= 3:
        tag = wire[p]
        rlen = int.from_bytes(wire[p + 1:p + 3], "little")
        if left < 3 + rlen:
            return b"", None
        if tag == KVSPACE_XVALUE_ATTR_TAG_LANGTYPE:
            langtype = bytes(wire[p + 3:p + 3 + rlen])
        elif tag == KVSPACE_XVALUE_ATTR_TAG_REALLEN:
            reallen = int.from_bytes(wire[p + 3:p + 3 + rlen], "little")
        p += 3 + rlen
        left -= 3 + rlen
    return langtype, reallen


def xvalue_head_effective_len(wire):
    """有效长度：layout=0 是 bodylen；layout=1 是 attr tag 2 的 reallen。"""
    if len(wire) < KVSPACE_XVALUE_HEAD_LEN:
        return None
    bodylen = int.from_bytes(wire[0:8], "little")
    if wire[12] != KVSPACE_XVALUE_LAYOUT_CAPACITY:
        return bodylen
    _, reallen = xvalue_head_scan(wire)
    return reallen


def bind(fn, args, restype):
    fn.argtypes = args
    fn.restype = restype


bind(lib.kvspaceConnect, [ctypes.c_char_p], ctypes.c_void_p)
bind(lib.kvspaceClose, [ctypes.c_void_p], None)
# 借用读：out 指向 kvspace 常驻空间，string_at 拷出后不 free。resolve 穿透 link。
bind(lib.kvspaceGet, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(U8P), U32P], ctypes.c_int)
# 写即构造：就地/新位置二选一原语，往返回的 body 偏移指针写字节。
bind(lib.kvspaceWriteInPlace, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_uint32, ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# Signature matches kvspace.h: (h, key, ref, storetype, body_len, body_cap, langtype, body, err, err_cap).
bind(lib.kvspaceWriteNewPlace, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint8, ctypes.c_uint8,
                                ctypes.c_uint32, ctypes.c_uint64, ctypes.c_char_p,
                                ctypes.POINTER(U8P), ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# 前缀枚举：ListLen 定计数 + 逐 idx ListAt 借用取名。
bind(lib.kvspaceListLen, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.POINTER(ctypes.c_int32)], ctypes.c_int)
bind(lib.kvspaceListAt, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_int, ctypes.c_int32,
                         U8P, ctypes.c_uint32, U32P], ctypes.c_int)
bind(lib.kvspaceDel, [ctypes.c_void_p, CCHARPP, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceDelTree, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
bind(lib.kvspaceMkindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint32, ctypes.c_char_p, ctypes.c_uint32], ctypes.c_int)
# New*: langtype is stored in inline attr (empty/None = untyped).
bind(lib.kvspaceNewInt64, [ctypes.c_int64, ctypes.c_char_p, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.kvspaceNewChar, [U8P, ctypes.c_uint32, ctypes.c_char_p, ctypes.POINTER(U8P), U32P], ctypes.c_int)
bind(lib.free, [ctypes.c_void_p], None)


def enc_int(v):
    out, n = U8P(), ctypes.c_uint32()
    if lib.kvspaceNewInt64(v, b"int64", ctypes.byref(out), ctypes.byref(n)) != 0:
        raise RuntimeError("kvspaceNewInt64 failed")
    data = ctypes.string_at(out, n.value)
    lib.free(out)  # allocated by frontend malloc
    return data


def new_str(s):
    b = s.encode()
    buf = (ctypes.c_uint8 * len(b)).from_buffer_copy(b)
    out, n = U8P(), ctypes.c_uint32()
    if lib.kvspaceNewChar(buf, len(b), b"char/utf8", ctypes.byref(out), ctypes.byref(n)) != 0:
        raise RuntimeError("kvspaceNewChar failed")
    data = ctypes.string_at(out, n.value)
    lib.free(out)  # allocated by frontend malloc
    return data


# kind 与 body 长度都从**同一条 wire**解出（`kvspaceGet` 的返回值），
# 不再调已删除的 kvspaceGetHead，也不维护第二份 head 结构体。
def read_kind(kv, key, data):
    if not data:
        return "", b""
    langtype, _ = xvalue_head_scan(data)
    kx = langtype.split(b"\0", 1)[0].decode()
    if kx.startswith(("*", "@")):
        kx = kx[1:]
    if kx.startswith("["):
        kx = kx[kx.index("]") + 1:]
    bl = xvalue_head_effective_len(data) or 0
    return kx, data[KVSPACE_XVALUE_HEAD_LEN:KVSPACE_XVALUE_HEAD_LEN + bl]


class KV:
    def __init__(self, path):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self.path = path
        self.kv = lib.kvspaceConnect(f"shm://{path}".encode())
        if not self.kv:
            raise RuntimeError(f"connect shm://{path} failed")

    def close(self):
        lib.kvspaceClose(self.kv)
        try:
            os.unlink(self.path)
        except FileNotFoundError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    # Write into returned body pointer; WriteNewPlace writes langtype with the header/attr region.
    def set(self, key, langtype, val):
        if len(val) < KVSPACE_XVALUE_HEAD_LEN:
            return
        bl = xvalue_head_effective_len(val)
        if bl is None:
            raise RuntimeError(f"write {key}: value head is not well-formed")
        # ref 由 langtype 的 * / @ 前缀隐含，不再从 head 位取。
        ref = 1 if langtype.startswith("*") else (2 if langtype.startswith("@") else 0)
        buf = (ctypes.c_uint8 * len(val)).from_buffer_copy(val)
        dst = U8P()
        err = ctypes.create_string_buffer(256)
        if lib.kvspaceWriteInPlace(self.kv, key.encode(), 1, bl, ctypes.byref(dst), err, 256) != 0:
            if lib.kvspaceWriteNewPlace(self.kv, key.encode(), ref, 0,
                                        bl, bl, langtype.encode(), ctypes.byref(dst), err, 256) != 0:
                raise RuntimeError(f"write {key}: {err.value.decode(errors='replace')}")
        if bl > 0 and dst:
            ctypes.memmove(dst, ctypes.byref(buf, KVSPACE_XVALUE_HEAD_LEN), bl)

    def get(self, key):
        out, n = U8P(), ctypes.c_uint32()
        lib.kvspaceGet(self.kv, key.encode(), 0, ctypes.byref(out), ctypes.byref(n))
        if not out or n.value == 0:
            return None
        return ctypes.string_at(out, n.value)

    def delete(self, key):
        keys = (ctypes.c_char_p * 1)(key.encode())
        lib.kvspaceDel(self.kv, keys, 1, None, 0)

    def deltree(self, prefix):
        lib.kvspaceDelTree(self.kv, prefix.encode(), None, 0)

    def mkindex(self, path):
        lib.kvspaceMkindex(self.kv, path.encode(), 0, None, 0)

    def list(self, prefix):
        count = ctypes.c_int32()
        if lib.kvspaceListLen(self.kv, prefix.encode(), 0, 1, ctypes.byref(count)) != 0 or count.value <= 0:
            return []
        names = []
        for i in range(count.value):
            out = (ctypes.c_uint8 * 1024)()
            n = ctypes.c_uint32()
            if lib.kvspaceListAt(self.kv, prefix.encode(), 0, 1, i, out, 1024, ctypes.byref(n)) == 0 and n.value > 0:
                names.append(ctypes.string_at(out, n.value).decode())
        return names


with KV("/tmp/kvspace_py.shm") as kv:
    kv.mkindex("/py/")

    kv.set("/py/a", "int64", enc_int(42))
    kv.set("/py/b", "char/utf8", new_str("你好"))

    for k in ["/py/a", "/py/b"]:
        kind, raw = read_kind(kv.kv, k, kv.get(k))
        if kind == "int64":
            val = struct.unpack("<q", raw)[0]
        else:
            val = raw.decode()
        print(f"{k}\t{kind}:{val}")

    ns = kv.list("/py/")
    print(f"list /py/: {sorted(ns)} (count={len(ns)})")

    kv.delete("/py/b")
    assert len(kv.list("/py/")) == 1
    kv.deltree("/py/")
    assert kv.list("/py/") == []

print("PASS python")
