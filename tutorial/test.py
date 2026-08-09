#!/usr/bin/env python3
"""kvspace-c tutorial test — 多语言 wrapper 验证"""

from __future__ import annotations
import os, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
TUTORIAL = ROOT / "tutorial"
SO = BUILD / "libkvspace-c.so"

passed = 0
failed = 0


def run(label: str, cmd: list[str], env: dict = None) -> bool:
    global passed, failed
    r = subprocess.run(cmd, capture_output=True, text=False, timeout=600,
                       cwd=str(ROOT), env={**os.environ, **(env or {})})
    if r.returncode == 0:
        print(f"PASS  {label}")
        passed += 1
        return True
    else:
        print(f"FAIL  {label}")
        if r.stdout:
            try: s = r.stdout.decode()[:300]
            except: s = repr(r.stdout[:100])
            print(f"  stdout: {s}")
        if r.stderr:
            try: s = r.stderr.decode()[:300]
            except: s = repr(r.stderr[:100])
            print(f"  stderr: {s}")
        failed += 1
        return False


def test_c():
    src = TUTORIAL / "01_basic.c"
    bin = BUILD / "tutorial" / "01_basic"
    if not run("C: compile",
        ["gcc", str(src), "-I", str(ROOT / "include"),
         "-L", str(BUILD), "-lkvspace-c", "-o", str(bin)]):
        return
    run("C: run", [str(bin)], {"LD_LIBRARY_PATH": str(BUILD)})


def test_cpp():
    bin = BUILD / "tutorial" / "02_cpp"
    if not run("C++: compile",
        ["g++", "-std=c++17", str(TUTORIAL / "02_cpp.cpp"),
         "-I", str(ROOT / "include"), "-L", str(BUILD),
         "-lkvspace-c", "-o", str(bin)]):
        return
    run("C++: run", [str(bin)], {"LD_LIBRARY_PATH": str(BUILD)})


def test_python():
    run("Python: test",
        [sys.executable, str(TUTORIAL / "03_python.py")],
        {"LD_LIBRARY_PATH": str(BUILD), "KVSPACE_C_SO": str(SO),
         "PYTHONPATH": str(ROOT / "py")})


def test_integrity():
    src = TUTORIAL / "05_integrity.c"
    bin = BUILD / "tutorial" / "05_integrity"
    if not run("C: integrity compile",
        ["gcc", str(src), "-I", str(ROOT / "include"),
         "-L", str(BUILD), "-lkvspace-c", "-o", str(bin)]):
        return
    run("C: integrity run", [str(bin)], {"LD_LIBRARY_PATH": str(BUILD)})

def test_rust():
    # Rust test (compile + run)
    bin = BUILD / "tutorial" / "04_rust"
    src = TUTORIAL / "04_rust.rs"
    run("Rust: compile",
        ["rustc", str(src), "-L", str(BUILD), "-l", "kvspace-c", "-o", str(bin)])
    run("Rust: run",
        [str(bin)], {"LD_LIBRARY_PATH": str(BUILD)})


if __name__ == "__main__":
    os.chdir(ROOT)
    os.system("cd build && cmake .. > /dev/null 2>&1 && make -j$(nproc) 2>&1 | tail -3")

    test_c()
    test_cpp()
    test_python()
    test_integrity()
    if shutil.which("rustc"):
        test_rust()
    else:
        print("SKIP  Rust (rustc not found)")

    total = passed + failed
    print(f"\n{passed}/{total} passed")
    sys.exit(0 if failed == 0 else 1)
