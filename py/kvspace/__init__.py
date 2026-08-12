"""
kvspace-c Python wrapper — ctypes FFI over libkvspace-c.so
"""

import ctypes, os, struct
from pathlib import Path
from typing import Optional

_SO = os.environ.get("KVSPACE_C_SO")
if not _SO:
    _SO = str(Path(__file__).parent.parent.parent / "build" / "libkvspace-c.so")
_lib = ctypes.CDLL(_SO)


def _bind(fn, argtypes, restype):
    fn.argtypes = argtypes
    fn.restype = restype


_bind(_lib.kvspace_open, [ctypes.c_char_p, ctypes.c_size_t], ctypes.c_void_p)
_bind(_lib.kvspace_close, [ctypes.c_void_p], None)
_bind(_lib.kvspace_get, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.POINTER(ctypes.c_int32)], ctypes.POINTER(ctypes.c_uint8))
_bind(_lib.kvspace_set, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32], ctypes.c_int)
_bind(_lib.kvspace_del, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspace_deltree, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspace_mkindex, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspace_list, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_bool, ctypes.c_int,
                           ctypes.POINTER(ctypes.c_void_p), ctypes.POINTER(ctypes.c_int32)], ctypes.c_int)
_bind(_lib.kvspace_extindex, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspace_delextindex, [ctypes.c_void_p, ctypes.c_char_p], ctypes.c_int)
_bind(_lib.kvspace_notify, [ctypes.c_void_p, ctypes.c_char_p, ctypes.POINTER(ctypes.c_uint8), ctypes.c_int32], ctypes.c_int)
_bind(_lib.kvspace_watch, [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int32, ctypes.POINTER(ctypes.c_int32)], ctypes.POINTER(ctypes.c_uint8))


# ── XValue TLV helpers ──────────────────────────────────────────

def _xv_encode(kind: str, raw: bytes, al: int = 1) -> bytes:
    kl = len(kind)
    return struct.pack(f"<B{kl}sii{len(raw)}s", kl, kind.encode(), al, len(raw), raw)


def _xv_decode(data: Optional[bytes]) -> tuple[str, int, bytes]:
    if not data:
        return ("", 0, b"")
    kl = data[0]
    off = 1 + kl
    al = struct.unpack_from("<i", data, off)[0]
    rl = struct.unpack_from("<i", data, off + 4)[0]
    raw = data[1 + kl + 8 : 1 + kl + 8 + rl]
    return (data[1:1 + kl].decode(), al, raw)


def xv_int(v: int) -> bytes:
    return _xv_encode("int64", struct.pack("<q", v))


def xv_float(v: float) -> bytes:
    return _xv_encode("float64", struct.pack("<d", v))


def xv_str(s: str) -> bytes:
    b = s.encode()
    return _xv_encode("string", b, al=len(b))


def xv_index() -> bytes:
    return _xv_encode("index", b"")


def xv_link(target: str) -> bytes:
    return _xv_encode("linkindex", target.encode())


def xv_ext(extpath: str) -> bytes:
    raw = ("…" + extpath).encode()
    return _xv_encode("extindex", raw)


# ── KVSpace ─────────────────────────────────────────────────────

class KVSpace:
    def __init__(self, path: str, data_size: int = 32768):
        try:
            os.unlink(path)
        except FileNotFoundError:
            pass
        self._path = path
        self._kv = _lib.kvspace_open(path.encode(), data_size)
        if not self._kv:
            raise RuntimeError(f"kvspace_open({path}) failed")

    def close(self):
        _lib.kvspace_close(self._kv)
        try:
            os.unlink(self._path)
        except FileNotFoundError:
            pass

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    # ── CRUD ─────────────────────────────────────────────────

    def get(self, key: str, resolve: bool = True) -> Optional[bytes]:
        ol = ctypes.c_int32(0)
        p = _lib.kvspace_get(self._kv, key.encode(), 1 if resolve else 0, ctypes.byref(ol))
        return ctypes.string_at(p, ol.value) if p and ol.value else None

    def set(self, key: str, val: bytes):
        _lib.kvspace_set(self._kv, key.encode(),
                         ctypes.cast(ctypes.c_char_p(val), ctypes.POINTER(ctypes.c_uint8)),
                         len(val))

    def delete(self, key: str):
        _lib.kvspace_del(self._kv, key.encode())

    def deltree(self, prefix: str):
        _lib.kvspace_deltree(self._kv, prefix.encode())

    # ── Directory ────────────────────────────────────────────

    def mkindex(self, path: str):
        _lib.kvspace_mkindex(self._kv, path.encode())

    def list(self, prefix: str, resolve: bool = True) -> list[str]:
        out = ctypes.c_void_p()
        oc = ctypes.c_int32(0)
        _lib.kvspace_list(self._kv, prefix.encode(), False, 1 if resolve else 0, ctypes.byref(out), ctypes.byref(oc))
        if oc.value == 0:
            return []
        ptrs = ctypes.cast(out, ctypes.POINTER(ctypes.c_char_p))
        return [ptrs[i].decode() for i in range(oc.value)]

    # ── Link / ExtIndex ──────────────────────────────────────

    def extindex(self, path: str, extpath: str):
        _lib.kvspace_extindex(self._kv, path.encode(), extpath.encode())

    def delextindex(self, path: str):
        _lib.kvspace_delextindex(self._kv, path.encode())
