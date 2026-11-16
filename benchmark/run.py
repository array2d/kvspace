#!/usr/bin/env python3
"""kvspace benchmark harness：跑 Q1–Q4 四组负载，产出一份 CSV。

只写 CSV，不画图、不生成 LaTeX——后者是 tools/export_latex.py 的职责。
这样「测」与「呈现」分开：改表格样式不用重跑，改负载不用动表格脚本。

CSV 列约定见 result/csv/SCHEMA.md。本 harness 产 suite=kvspace 的行。

设计约束（与论文 08 章一致）：
- 缺失的对照实现写 `notes=skipped: <原因>`，**不产伪造行**；
- 每组至少 REPEAT 次独立重复，取分位时合并全部样本（不取各次最优）；
- 计时前预热 WARMUP 次，预热不计入统计；
- 数值一律来自实测；拿不到就整行不写，让导出侧显示 TODO(实测)。

用法：
    python3 benchmark/run.py --suite q1
    python3 benchmark/run.py --suite q1,q3 --repeat 5
    python3 benchmark/run.py --list
"""
import argparse
import csv
import os
import platform
import socket
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent
KVLANG_ROOT = ROOT.parent
RESULT_DIR = ROOT / "result" / "csv"

                                         
REPEAT = 3
WARMUP = 100

                                                  
VALUE_SIZES = (1, 8, 64, 4096)
DEPTHS = (1, 4, 8)
THREADS = (1, 4, 16)
DIRS = (1, 8)

FIELDS = ["suite", "implementation", "workload", "metric", "value", "unit",
          "version", "timestamp", "notes",
          "value_size", "key_depth", "threads", "percentile"]


def utc_now():
    return datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def stamp():
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def version():
    """被测实现的版本：优先 git describe，取不到就 unknown（不猜）。"""
    out = subprocess.run(["git", "-C", str(KVLANG_ROOT), "describe", "--tags",
                          "--always", "--dirty"], capture_output=True, text=True)
    return out.stdout.strip() or "unknown"


def env_note():
    """环境指纹：同一条 CSV 里的数字只有配上这些才有意义。"""
    return (f"cpu={platform.processor() or 'unknown'};"
            f"cores={os.cpu_count()};"
            f"host={socket.gethostname()};"
            f"python={platform.python_version()}")


def quantile(samples, q):
    """最近秩法取分位。samples 必须非空。"""
    ordered = sorted(samples)
    if not ordered:
        return None
    index = int(round(q * (len(ordered) - 1)))
    return ordered[max(0, min(index, len(ordered) - 1))]


def emit(writer, *, implementation, workload, metric, value, unit,
         notes="", value_size="", key_depth="", threads="", percentile="-"):
    writer.writerow({
        "suite": "kvspace",
        "implementation": implementation,
        "workload": workload,
        "metric": metric,
        "value": value,
        "unit": unit,
        "version": version(),
        "timestamp": utc_now(),
        "notes": notes,
        "value_size": value_size,
        "key_depth": key_depth,
        "threads": threads,
        "percentile": percentile,
    })


def suspended(reason):
    """某个对照实现跑不了：按 SCHEMA 写 skipped，不产数值行。

    这不是「跑出 0」，所以**不能**写成 value=0——那会把「没测」说成「测出来是零」。
    """
    print(f"skip: {reason}", file=sys.stderr)


def suite_q1(writer, args):
    """Q1：小值（8 字节）单次写入的端到端成本。

    只测 kvspace 自身。其他实现（文件系统 / Redis / mmap+哈希）由各自的驱动提供，
    本 harness 不做——把它们塞进来会让「被测对象」与「对照组」混在一个脚本里，
    改对照组就要动被测脚本。
    """
    driver = KVLANG_ROOT / "bin" / "kvlangrun"
    if not driver.exists():
        suspended(f"{driver} 不存在")
        emit(writer, implementation="kvspace", workload="write-8B",
             metric="p50", value="", unit="ns",
             notes="skipped: driver not built", value_size="8", threads="1")
        return
    suspended("Q1 驱动接线待实现（需要 kvlangrun 的 write 子命令）")
    emit(writer, implementation="kvspace", workload="write-8B",
         metric="p50", value="", unit="ns",
         notes="skipped: driver wiring not implemented",
         value_size="8", threads="1")


def suite_q2(writer, args):
    """Q2：地址稳定性（布尔）与零拷贝读收益。"""
    suspended("Q2 驱动接线待实现")
    for case in ("address-stable-after-grow", "zero-copy-64B", "zero-copy-4096B"):
        emit(writer, implementation="kvspace", workload=case,
             metric="value", value="", unit="",
             notes="skipped: driver wiring not implemented")


def suite_q3(writer, args):
    """Q3：线程数 × 目录数下的 create/delete 吞吐与延迟。"""
    suspended("Q3 驱动接线待实现")
    for threads in THREADS:
        for dirs in DIRS:
            emit(writer, implementation="kvspace",
                 workload=f"create-delete/t={threads}/d={dirs}",
                 metric="ops_per_second", value="", unit="ops/s",
                 notes="skipped: driver wiring not implemented",
                 threads=str(threads), key_depth=str(dirs))


def suite_q4(writer, args):
    """Q4：崩溃恢复三态（值正确 / 值缺失 / 值不可解释）。"""
    suspended("Q4 驱动接线待实现")
    for scenario in ("kill-9-then-reopen", "restart-process", "restart-machine"):
        for metric in ("correct", "missing", "unreadable"):
            emit(writer, implementation="kvspace", workload=scenario,
                 metric=metric, value="", unit="count",
                 notes="skipped: driver wiring not implemented")


SUITES = {"q1": suite_q1, "q2": suite_q2, "q3": suite_q3, "q4": suite_q4}


def main():
    parser = argparse.ArgumentParser(description="kvspace benchmark harness")
    parser.add_argument("--suite", default="q1,q2,q3,q4",
                        help="逗号分隔，取值 q1/q2/q3/q4")
    parser.add_argument("--repeat", type=int, default=REPEAT)
    parser.add_argument("--warmup", type=int, default=WARMUP)
    parser.add_argument("--list", action="store_true", help="列出 suite 后退出")
    parser.add_argument("--no-write", action="store_true", help="只跑不落盘")
    args = parser.parse_args()

    if args.list:
        for name, func in SUITES.items():
            print(f"{name}: {func.__doc__.splitlines()[0].strip()}")
        return 0

    names = [n.strip() for n in args.suite.split(",") if n.strip()]
    unknown = [n for n in names if n not in SUITES]
    if unknown:
        print(f"unknown suite: {unknown}", file=sys.stderr)
        return 2

    ver = version()
    out_path = RESULT_DIR / f"results-kvspace-{ver}-{stamp()}.csv"
    RESULT_DIR.mkdir(parents=True, exist_ok=True)
    if not args.no_write:
        with out_path.open("w", newline="", encoding="utf-8") as handle:
            handle.write(f"# generated by benchmark/run.py at {utc_now()} "
                         f"version={ver}\n")
            handle.write(f"# {env_note()}\n")
            writer = csv.DictWriter(handle, fieldnames=FIELDS)
            writer.writeheader()
            for name in names:
                SUITES[name](writer, args)
        print(f"→ {out_path}")
    else:
        writer = csv.DictWriter(open(os.devnull, "w", newline=""), fieldnames=FIELDS)
        for name in names:
            SUITES[name](writer, args)
        print("(no-write)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
