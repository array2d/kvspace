#!/usr/bin/env python3
"""malloc benchmark runner —— 编译并运行 malloc_bench，结果落 result/csv/。

硬规则（见 result/csv/SCHEMA.md）：
  - CSV 只由本脚本写，数值不手改；
  - 缺失的对照库在 notes 写 `skipped: <库> not installed`，不伪造行；
  - 每份 CSV 带生成元信息：CSV 头部注释行 + 同名 meta.json。

用法：
    python3 benchmark/tools/run_malloc_bench.py [--scale N] [--no-build]

时间戳与版本号全部由脚本生成，不接受手写。
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import platform
import shutil
import subprocess
import sys
import tempfile
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
KVSPACE_ROOT = HERE.parent.parent
RESULT_CSV = KVSPACE_ROOT / "benchmark" / "result" / "csv"

                                           
SBO_ROOT = Path(
    os.environ.get(
        "SLOTSBOXMALLOC_ROOT",
        Path.home() / "github.com" / "array2d" / "slotsboxmalloc",
    )
)
BLOCKMALLOC_ROOT = SBO_ROOT.parent / "blockmalloc"


def utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def git_short_head(repo: Path) -> str:
    """commit 短哈希：脚本取，不手写。"""
    try:
        out = subprocess.run(
            ["git", "-C", str(repo), "rev-parse", "--short=7", "HEAD"],
            capture_output=True, text=True, check=True,
        )
        return out.stdout.strip()
    except (subprocess.CalledProcessError, FileNotFoundError):
        return "nogit"


def cpu_model() -> str:
    try:
        for line in Path("/proc/cpuinfo").read_text().splitlines():
            if line.startswith("model name"):
                return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or "unknown"


def governor() -> str:
    p = Path("/sys/devices/system/cpu/cpu0/cpufreq/scaling_governor")
    try:
        return p.read_text().strip()
    except OSError:
        return "unreadable"


def file_identity(path: Path) -> dict:
    """被测 .so 的身份：路径、mtime、sha256。

    陈旧产物是这个 harness 已经踩过一次的坑（9 月的 .so 配 10 月的头，
    初始化静默成功但写出非法状态）。把身份钉进 meta.json，
    事后再看结果时能立刻排除「拿旧库出的数」。"""
    if not path.exists():
        return {"path": str(path), "exists": False}
    digest = hashlib.sha256(path.read_bytes()).hexdigest()
    st = path.stat()
    return {
        "path": str(path),
        "exists": True,
        "size_bytes": st.st_size,
        "mtime_utc": datetime.fromtimestamp(st.st_mtime, timezone.utc)
        .strftime("%Y%m%dT%H%M%SZ"),
        "sha256": digest,
    }


def resolved_library_identity(soname: str) -> dict:
    """按 soname 经 ldconfig 解析到实际 .so，记录路径/sha256/包版本。

    记 soname 到包版本这两层：soname 是运行期真正加载的东西，包版本是「哪次
    apt 装的」——两者都可能不同（同 soname 换实现）。缺一个都不是完整身份。
    """
    result: dict = {"soname": soname, "resolved": False}
    try:
        cache = subprocess.run(
            ["ldconfig", "-p"], capture_output=True, text=True, check=True
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return result
    path = None
    for line in cache.splitlines():
        if soname in line and "=>" in line:
            path = line.split("=>")[-1].strip()
            break
    if not path:
        return result
    identity = file_identity(Path(path))
    identity["resolved"] = identity.get("exists", False)
    identity["soname"] = soname
                                              
                                                                  
                                                 
    identity["provided_by"] = None
    identity["package_version"] = None
    try:
                                                                    
                                              
                                  
        canonical = str(Path(path).resolve())
        out = subprocess.run(
            ["dpkg-query", "-S", canonical], capture_output=True, text=True, check=True
        ).stdout.strip()
        package = out.split(":")[0].split(",")[0].strip() if out else None
        if package:
            identity["provided_by"] = package
            version = subprocess.run(
                ["dpkg-query", "-W", "-f=${Version}", package],
                capture_output=True, text=True, check=True,
            ).stdout.strip()
            identity["package_version"] = version or None
    except (subprocess.CalledProcessError, FileNotFoundError):
        pass
    return identity


def detect_missing_libs() -> list[str]:
    """对照库是否安装：查动态链接器缓存，不猜。"""
    wanted = ["jemalloc", "mimalloc", "tcmalloc"]
    try:
        cache = subprocess.run(
            ["ldconfig", "-p"], capture_output=True, text=True, check=True
        ).stdout
    except (subprocess.CalledProcessError, FileNotFoundError):
        return wanted
    return [w for w in wanted if w not in cache]


def build(binary: Path, allocator: str | None = None) -> list[str]:
    """编译一个 harness 变体。返回实际使用的编译命令，写进 meta.json。

    `allocator=None` 是默认（glibc）；否则按名字加 `-l<lib>` 与
    `-DBENCH_LINKED_ALLOCATOR=<name>`，让本二进制由该库提供 malloc/free。

    **不用 dlopen、不用 LD_PRELOAD**：jemalloc 的静态 TLS 不支持前者（实测
    `cannot allocate memory in static TLS block`），后者会污染同进程的其它列。
    链时替换让每个变体在自己的进程里独占 malloc，四组数同口径。
    """
    include_dirs = [SBO_ROOT / "include", BLOCKMALLOC_ROOT / "include"]
    lib_dir = SBO_ROOT / "build"
    if not (lib_dir / "libslotsboxmalloc.so").exists():
        raise SystemExit(
            f"缺库：{lib_dir}/libslotsboxmalloc.so 不存在；"
            f"先在 {SBO_ROOT} 跑 make"
        )
    cmd = [
        "gcc", "-O3", "-DNDEBUG", "-std=c11",
        "-I", str(include_dirs[0]),
        "-I", str(include_dirs[1]),
        str(HERE / "malloc_bench.c"),
        "-L", str(lib_dir), "-lslotsboxmalloc",
        "-ldl",                     
    ]
    if allocator:
        cmd += [f"-DBENCH_LINKED_ALLOCATOR=\"{allocator}\"", f"-l{allocator}"]
                                                            
                                                      
                              
        cmd += ["-Wl,--no-as-needed"]
    cmd += ["-o", str(binary)]
    subprocess.run(cmd, check=True)
    return cmd


def main() -> int:
    ap = argparse.ArgumentParser(description="malloc benchmark runner")
    ap.add_argument("--scale", type=float, default=1.0,
                    help="workload 规模缩放（默认 1.0 = 全量）")
    ap.add_argument("--no-build", action="store_true", help="跳过编译")
    ap.add_argument("--out-dir", default=str(RESULT_CSV))
    args = ap.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)

    version = git_short_head(KVSPACE_ROOT)
    sbo_version = git_short_head(SBO_ROOT)
    stamp = utc_stamp()
    missing = detect_missing_libs()

    csv_path = out_dir / f"results-malloc-{version}-{stamp}.csv"
    meta_path = out_dir / f"results-malloc-{version}-{stamp}.meta.json"

                                
                                                      
    lib_path = SBO_ROOT / "build" / "libslotsboxmalloc.so"
    hdr_path = SBO_ROOT / "include" / "slotsboxmalloc" / "slotsboxobj.h"
    if lib_path.exists() and hdr_path.exists():
        if lib_path.stat().st_mtime < hdr_path.stat().st_mtime:
            raise SystemExit(
                f"闸：{lib_path.name} 比 {hdr_path.name} 旧"
                f"（{datetime.fromtimestamp(lib_path.stat().st_mtime)} < "
                f"{datetime.fromtimestamp(hdr_path.stat().st_mtime)}）；"
                f"先在 {SBO_ROOT} 跑 make"
            )

                           
                                             
     
                                       
                                          
    variants: list[tuple[str | None, str]] = [
        (None, "glibc"),
        ("jemalloc", "jemalloc"),
        ("mimalloc", "mimalloc"),
        ("tcmalloc", "tcmalloc"),
    ]

    tmp_dir = Path(tempfile.mkdtemp(prefix="mallocbench-"))
    env = dict(os.environ)
    env["LD_LIBRARY_PATH"] = (
        f"{SBO_ROOT / 'build'}:{env.get('LD_LIBRARY_PATH', '')}"
    )
    env["BENCH_SCALE"] = str(args.scale)

    outputs: list[dict] = []
    for library, label in variants:
        binary = tmp_dir / f"malloc_bench_{label}"
        compile_cmd = [] if args.no_build else build(binary, library)
        if args.no_build and not binary.exists():
            raise SystemExit(f"--no-build 但 {binary} 不存在")
                                                  
        variant_csv = out_dir / f"results-malloc-{version}-{stamp}-{label}.csv"
        variant_meta = out_dir / f"results-malloc-{version}-{stamp}-{label}.meta.json"
        cmd = ["taskset", "-c", "0", str(binary), str(variant_csv),
               str(variant_meta), version, stamp]
        proc = subprocess.run(cmd, capture_output=True, text=True, env=env,
                              timeout=7200)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            raise SystemExit(f"malloc_bench({label}) 退出码 {proc.returncode}")
        outputs.append({
            "variant": label,
            "linked_allocator": library,
            "csv": str(variant_csv),
            "meta": str(variant_meta),
            "compile_command": compile_cmd,
            "run_command": cmd,
        })

                                                
    csv_path = Path(outputs[0]["csv"])
    meta_path = Path(outputs[0]["meta"])

                                   
                                                  
                                     
    for output in outputs:
        variant_path = Path(output["csv"])
        linked = output["linked_allocator"] or "glibc"
        stamp_line = (
            f"# generated by benchmark/tools/run_malloc_bench.py at {stamp} "
            f"version={version}\n"
            f"# slotsboxmalloc HEAD={sbo_version} scale={args.scale} "
            f"cpp=O3 -DNDEBUG -std=c11 linked=libslotsboxmalloc.so "
            f"variant={output['variant']} linked_allocator={linked}\n"
        )
        variant_path.write_text(stamp_line + variant_path.read_text())

    meta = {
        "generated_by": "benchmark/tools/run_malloc_bench.py",
        "timestamp": stamp,
        "version": version,
        "slotsboxmalloc_version": sbo_version,
        "scale": args.scale,
                                    
                                              
        "variants": [
            {
                "variant": output["variant"],
                "linked_allocator": output["linked_allocator"] or "glibc",
                                                
                "link_mode": (
                    f"linked: {output['linked_allocator']}"
                    if output["linked_allocator"]
                    else "linked: libc (default)"
                ),
                "compile_command": output["compile_command"],
                "run_command": output["run_command"],
                "csv": output["csv"],
            }
            for output in outputs
        ],
        "tested_library": file_identity(SBO_ROOT / "build" / "libslotsboxmalloc.so"),
                                              
                                           
        "comparison_libraries": {
            name: resolved_library_identity(soname)
            for name, soname in (
                ("jemalloc", "libjemalloc.so.2"),
                ("mimalloc", "libmimalloc.so.2"),
                ("tcmalloc", "libtcmalloc.so.4"),
            )
        },
        "header": file_identity(
            SBO_ROOT / "include" / "slotsboxmalloc" / "slotsboxobj.h"
        ),
        "run_command": cmd,
        "host": {
            "nproc": os.cpu_count(),
            "cpu_model": cpu_model(),
            "governor": governor(),
            "kernel": platform.release(),
            "libc": " ".join(platform.libc_ver()).strip() or "unknown",
        },
        "missing_reference_libs": missing,
        "notes": [
            "single-threaded; taskset -c 0",
            "cross-process shared-memory scenarios are out of scope for the malloc suite",
            "fragmentation_ratio only defined for slotsbox (free_slots / longest_run)",
        ],
    }
    meta_path.write_text(json.dumps(meta, indent=2, ensure_ascii=False) + "\n")

                                 
                                                 
                                                
    print(f"variants={len(outputs)}")
    for output in outputs:
        variant_path = Path(output["csv"])
        rows = sum(1 for _ in variant_path.open()) - 3              
        print(f"  {output['variant']:>10}: {variant_path}  rows={rows}")
    print(meta_path)
    if missing:
        print(f"dlopen_unavailable={missing}（正式数据来自链时变体，此列仅表示"
              f"交叉验证不可用）")
    shutil.rmtree(tmp_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
