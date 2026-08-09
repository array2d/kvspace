#!/usr/bin/env python3
"""03_python — 使用 py/kvspace 包的示例"""

import sys
sys.path.insert(0, "../py")

from kvspace import KVSpace, xv_int, xv_float, xv_str, xv_index, xv_link, xv_ext, _xv_decode

with KVSpace("/tmp/kvspace_py.shm") as kv:
    kv.mkindex("/py/")

    kv.set("/py/a", xv_int(42))
    kv.set("/py/b", xv_float(3.14))
    kv.set("/py/c", xv_str("你好"))

    for k in ["/py/a", "/py/b", "/py/c"]:
        v = kv.get(k)
        kind, al, raw = _xv_decode(v)
        if kind == "int64":
            import struct
            val = struct.unpack_from("<q", raw)[0]
        elif kind == "float64":
            val = f"{struct.unpack_from('<d', raw)[0]:.2f}"
        else:
            val = raw.decode()
        print(f"{k}\t{kind}:{val}")

    ns = kv.list("/py/")
    print(f"list /py/: {sorted(ns)} (count={len(ns)})")

    kv.delete("/py/b")
    assert len(kv.list("/py/")) == 2
    kv.deltree("/py/")
    assert kv.list("/py/") == []

    print("PASS python")
