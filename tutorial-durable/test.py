#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Run tutorial-durable/*.sh and compare against expected header comments;
and verify write flush on next op and exit (issue #23)."""

import ctypes
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent  # kvspace/
WS = ROOT.parent  # array2d/


def find_first(env_key, candidates):
    override = os.environ.get(env_key)
    if override:
        return Path(override)
    for c in candidates:
        p = Path(c)
        if p.exists():
            return p
    return Path(candidates[0])


FRONTEND_SO = str(find_first("KVSPACE_SO", [
    ROOT / "build" / "libkvspace.so.1",
    "/usr/lib/kvspace/libkvspace.so.1",
]))
KVSPACE_BIN = str(find_first("KVSPACE_BIN", [
    ROOT / "cli" / "target" / "release" / "kvspace",
    os.path.expanduser("~/.local/bin/kvspace"),
    shutil.which("kvspace") or "/usr/bin/kvspace",
]))


def extract_expected(script):
    """从脚本头部 # expected: ... # /end 提取预期输出行。"""
    lines = []
    in_block = False
    with open(script) as f:
        for raw in f:
            line = raw.rstrip('\n')
            if in_block:
                if line == '# /end':
                    break
                if line.startswith('# '):
                    lines.append(line[2:])
                elif line == '#':
                    lines.append('')
            elif line == '# expected:':
                in_block = True
    return lines


def run_script(script):
    """Run script (each cleans up its own prefix via deltree without clearing shared redis) and return stdout lines."""
    env = os.environ.copy()
    env['KVSPACE_BIN'] = KVSPACE_BIN
    if 'KVSPACE' not in env:
        with tempfile.TemporaryDirectory(prefix='kvspace-tut-') as tmp:
            env['KVSPACE'] = f'fs://{tmp}/store'
            r = subprocess.run(['bash', script], capture_output=True, text=True, timeout=30, env=env)
            return r.stdout.rstrip('\n').split('\n') if r.stdout.strip() else [], r
    # Safe to clear explicit fs:// paths; redis:// relies on per-script deltree prefix cleanup.
    if env['KVSPACE'].startswith('fs://'):
        subprocess.run([KVSPACE_BIN, 'clear'], capture_output=True, timeout=10, env=env)
    r = subprocess.run(['bash', script], capture_output=True, text=True, timeout=30, env=env)
    return (r.stdout.rstrip('\n').split('\n') if r.stdout.strip() else []), r


def test_script(script):
    expected = extract_expected(script)
    actual, r = run_script(script)
    if r.returncode == 0 and expected == actual:
        print(f'PASS  {os.path.basename(script)}')
        return True
    print(f'FAIL  {os.path.basename(script)} (rc={r.returncode})')
    print(f'  expected ({len(expected)} lines): {expected[:3]}...' if len(expected) > 3 else f'  expected: {expected}')
    print(f'  actual   ({len(actual)} lines):   {actual[:3]}...' if len(actual) > 3 else f'  actual:   {actual}')
    if r.stderr.strip():
        print(f'  stderr: {r.stderr.strip()[:300]}')
    return False


# ── 写侧落盘时机（issue #23）─────────────────────────────────────────
#
# 写把 XValue 字节攒在句柄内（body 由调用方在返回后填，返回前无法落盘）：下一次 op 落盘上一笔
# （读也是 op），另一个句柄随即可见；漏调 Close 而正常退出时由前端退出兜底落盘。子进程必须
# 独立——它正是「不 Close 就退出」的那一个。

CHILD = r'''
# 只用新 ABI 读一笔，判断上一进程的写是否已落地。
# 写入由 CLI 做（durable 没有进程内值区，realloc 拿不到 offset，写路径只存在于
# shm）；本探针要验的是「落盘时机」，读得到即说明写已可见。
import ctypes, sys
lib = ctypes.CDLL(sys.argv[1])
lib.kvspaceConnect.restype = ctypes.c_void_p
lib.kvspaceConnect.argtypes = [ctypes.c_char_p]
lib.kvspaceResolve.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_uint64,
                               ctypes.POINTER(ctypes.c_uint64), ctypes.c_char_p,
                               ctypes.c_uint32]
err = ctypes.create_string_buffer(256)
h1 = lib.kvspaceConnect(sys.argv[2].encode())
assert h1, "connect failed"
off = ctypes.c_uint64()
rc = lib.kvspaceResolve(h1, sys.argv[3].encode(), 0, ctypes.byref(off), err, 256)
print("next-op-visible", rc == 0)
sys.exit(0)
'''


def redis_reachable(dsn):
    import socket
    addr = dsn.removeprefix("redis://").split("/", 1)[0]
    host, _, port = addr.partition(":")
    try:
        with socket.create_connection((host or "127.0.0.1", int(port or 6379)), timeout=0.3):
            return True
    except OSError:
        return False


def test_exit_flush():
    """写侧落盘时机：下一次 op 即落盘上一笔；漏调 Close 正常退出不丢最后一笔。

    写入统一用 CLI（durable 无进程内值区，新 ABI 的写路径只存在于 shm）；
    读回用新 ABI 的 kvspaceResolve，验「另一进程看得到」。
    """
    if not Path(FRONTEND_SO).exists():
        print(f'FAIL  exit-flush (缺前端 {FRONTEND_SO})')
        return False
    env = os.environ.copy()
    pfx = f'/flushprobe-{os.getpid()}'
    keys = [f'{pfx}/a', f'{pfx}/b', f'{pfx}/c', f'{pfx}/d']
    with tempfile.TemporaryDirectory(prefix="kvspace-flush-") as tmp:
        cases = [
            ('fs', f'fs://{tmp}/fs'),
            ('shm', f'shm://{tmp}/probe.shm'),
        ]
        rds_dsn = env.get('KVSPACE', 'redis://127.0.0.1:6379')
        if rds_dsn.startswith('redis://') and redis_reachable(rds_dsn):
            cases.insert(0, ('redis', rds_dsn))
        ok = True
        for label, dsn in cases:
            cenv = {**env, 'KVSPACE': dsn}
            subprocess.run([KVSPACE_BIN, 'deltree', f'{pfx}/'], capture_output=True, env=cenv)
            for k in keys[:3]:
                subprocess.run([KVSPACE_BIN, 'set', k, 'char/utf8:v'], capture_output=True, env=cenv)
            child = subprocess.run([sys.executable, '-c', CHILD, FRONTEND_SO, dsn, f'{pfx}/c'],
                                   capture_output=True, text=True, timeout=30, env=env)
            seen = 'next-op-visible True' in child.stdout
            got = subprocess.run([KVSPACE_BIN, 'get', *keys], capture_output=True, text=True,
                                 timeout=10, env=cenv)
            if label == 'redis':
                subprocess.run([KVSPACE_BIN, 'deltree', f'{pfx}/'], capture_output=True,
                               timeout=10, env=cenv)
            kept = [l for l in got.stdout.splitlines() if 'char/utf8:v' in l]
            if seen and len(kept) == len(keys):
                print(f'PASS  exit-flush {label}')
            else:
                print(f'FAIL  exit-flush {label} (next-op-visible={seen}, 落地 {len(kept)}/{len(keys)})')
                if child.stderr:
                    print(f'  child stderr: {child.stderr.strip()}')
                if got.stderr:
                    print(f'  cli stderr: {got.stderr.strip()}')
                ok = False
        return ok


def main():
    here = os.path.dirname(os.path.abspath(__file__))
    scripts = sorted(
        os.path.join(here, f) for f in os.listdir(here)
        if f.endswith('.sh')
    )
    if not scripts:
        print('no scripts found')
        sys.exit(1)

    results = [test_script(s) for s in scripts]
    results.append(test_exit_flush())
    passed = sum(results)
    print(f'\n{passed}/{len(results)} passed')
    sys.exit(0 if passed == len(results) else 1)


if __name__ == '__main__':
    main()
