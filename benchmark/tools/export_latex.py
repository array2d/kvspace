#!/usr/bin/env python3
"""把 benchmark CSV 导出成论文用 LaTeX 表格片段。

论文侧用 \\input 引入生成的 .tex；本脚本只产 tabular，不写 \\usepackage、不定义颜色。

四张表对应论文 08 章（评测计划）的 Q1–Q4：

| 表 | 文件 | 列 |
|---|---|---|
| Q1 写入延迟 | q1-write-latency.tex | implementation, value_size, p50, p99 |
| Q2 地址稳定性 | q2-address-stability.tex | implementation, case, result |
| Q3 线程×目录 | q3-thread-dir.tex | threads, dirs, ops_per_second, p50, p99 |
| Q4 三态恢复 | q4-recovery-tristate.tex | scenario, correct, missing, unreadable |

**缺数据显示 TODO(实测)**，不猜、不插值、不拿相邻行代替。缺基线按 SCHEMA 的约定在
notes 里是 `skipped: ...`，本脚本把它原样标成 `skipped`，不当成 0。

用法：
    python3 benchmark/tools/export_latex.py                 # 读全部 CSV，写四张表
    python3 benchmark/tools/export_latex.py --csv a.csv     # 只读指定文件
    python3 benchmark/tools/export_latex.py --check         # 只报告缺哪些格子，不写文件
"""
import argparse
import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CSV_DIR = ROOT / "result" / "csv"
OUT_DIR = ROOT / "result" / "fig" / "tables"

MISSING = "TODO(实测)"
SKIPPED = "skipped"

                                               
TABLES = {
    "q1-write-latency": {
        "caption": "Q1：8 字节值单次写入延迟",
        "label": "tab:q1-write-latency",
        "header": ["实现", "$p50$ (ns)", "$p99$ (ns)"],
        "columns": "lrr",
    },
    "q2-address-stability": {
        "caption": "Q2：地址稳定性与零拷贝读",
        "label": "tab:q2-address-stability",
        "header": ["实现", "用例", "结果"],
        "columns": "lll",
    },
    "q3-thread-dir": {
        "caption": "Q3：线程数 $\\times$ 目录数",
        "label": "tab:q3-thread-dir",
        "header": ["线程", "目录", "吞吐 (ops/s)", "$p50$ (ns)", "$p99$ (ns)"],
        "columns": "rrrrr",
    },
    "q4-recovery-tristate": {
        "caption": "Q4：崩溃恢复三态",
        "label": "tab:q4-recovery-tristate",
        "header": ["场景", "值正确", "值缺失", "值不可解释"],
        "columns": "lrrr",
    },
}


def load_rows(paths):
    """读全部 CSV 行。跳过 SCHEMA 里约定的 `#` 注释行。"""
    rows = []
    for path in paths:
        with path.open(newline="", encoding="utf-8") as handle:
            lines = (line for line in handle if not line.startswith("#"))
            rows.extend(csv.DictReader(lines))
    return rows


def escape(text):
    """把 CSV 文本转成 LaTeX 安全形式。

    只处理会破坏 tabular 的字符：`_`（下标）、`&`（列分隔）、`%`（注释）、
    `#`、`$`、`{`、`}`。工作负载名与 metric 名里带 `_` 是常态（`recovery_ok`、
    `ops_per_second`），不转义会让整张表编译失败。
    """
    if text is None:
        return ""
    for char in ("\\", "&", "%", "#", "$", "{", "}", "_"):
        text = text.replace(char, "\\" + char)
    return text


def cell(row, column):
    """取一个单元格的展示文本。

    缺失 → TODO(实测)；notes 里带 skipped → skipped。
    两者都不能被当成 0——那是把「没测」说成「测出来是零」。
    """
    if row is None:
        return MISSING
    notes = (row.get("notes") or "").strip()
    if notes.startswith(SKIPPED):
        return SKIPPED
    value = (row.get(column) or "").strip()
    return escape(value) if value else MISSING


def index_by(rows, *keys):
    """按给定列组合建索引；同键重复时保留第一条（模型层不替数据选优）。"""
    table = {}
    for row in rows:
        key = tuple((row.get(k) or "").strip() for k in keys)
        table.setdefault(key, row)
    return table


def table_q1(rows):
    """Q1：每个实现一行，按 value_size 取 8 字节那一档。"""
    targets = [r for r in rows if (r.get("value_size") or "").strip() == "8"]
    index = index_by(targets, "implementation")
    implementations = sorted({k[0] for k in index}) or [""]
    body = []
    for impl in implementations:
        row = index.get((impl,))
        body.append([escape(impl) or MISSING, cell(row, "p50"), cell(row, "p99")])
                                                      
                                    
    for row in targets:
        unit = (row.get("unit") or "").strip()
        if unit and unit != "ns":
            print(f"warn: q1 行 unit={unit}，表头假定 ns；请核对",
                  file=sys.stderr)
    return body


def table_q2(rows):
    """Q2：每个 (实现, 用例) 一行。结果列可能是布尔或延迟数值。"""
    targets = [r for r in rows if r.get("suite") == "kvspace"
               and (r.get("percentile") or "").strip() in ("", "-")
               and (r.get("metric") or "").strip() in ("recovery_ok", "address_stable", "p50")]
    index = index_by(targets, "implementation", "workload")
    body = []
    for (impl, workload), row in sorted(index.items()):
        metric = (row.get("metric") or "").strip()
        result = cell(row, "value")
        if result not in (MISSING, SKIPPED) and metric:
            result = f"{result} ({escape(metric)})"
        body.append([escape(impl) or MISSING, escape(workload) or MISSING, result])
    return body


def table_q3(rows):
    """Q3：线程 × 目录 的二维表，每个 (threads, key_depth) 一行。"""
    targets = [r for r in rows
               if (r.get("metric") or "").strip() in ("ops_per_second", "p50", "p99")]
    index = index_by(targets, "threads", "key_depth", "metric")
    pairs = sorted({(k[0], k[1]) for k in index})
    body = []
    for threads, depth in pairs:
        body.append([
            escape(threads) or MISSING,
            escape(depth) or MISSING,
            cell(index.get((threads, depth, "ops_per_second")), "value"),
            cell(index.get((threads, depth, "p50")), "value"),
            cell(index.get((threads, depth, "p99")), "value"),
        ])
    return body


def table_q4(rows):
    """Q4：每个场景一行，三态各一格。

    三态来自 CSV 里 metric ∈ {correct, missing, unreadable} 的三个值；
    缺哪个就哪个格子 TODO(实测)——不允许「大部分还在」这种合并写法。
    """
    targets = [r for r in rows
               if (r.get("metric") or "").strip() in ("correct", "missing", "unreadable")]
    index = index_by(targets, "workload", "metric")
    scenarios = sorted({k[0] for k in index})
    body = []
    for scenario in scenarios:
        body.append([
            escape(scenario) or MISSING,
            cell(index.get((scenario, "correct")), "value"),
            cell(index.get((scenario, "missing")), "value"),
            cell(index.get((scenario, "unreadable")), "value"),
        ])
    return body


BUILDERS = {
    "q1-write-latency": table_q1,
    "q2-address-stability": table_q2,
    "q3-thread-dir": table_q3,
    "q4-recovery-tristate": table_q4,
}


def render(name, spec, body):
    """渲染一张表：只有 tabular + 外层 table 环境，不带任何宏包或颜色。"""
    lines = [
        "% 由 benchmark/tools/export_latex.py 生成，勿手改。",
        f"% 数据源：{CSV_DIR.relative_to(ROOT)}",
        "\\begin{table}[h]",
        "\\centering",
        "\\small",
        f"\\begin{{tabular}}{{{spec['columns']}}}",
        "\\toprule",
        " & ".join(spec["header"]) + " \\\\",
        "\\midrule",
    ]
    if body:
        for row in body:
            lines.append(" & ".join(row) + " \\\\")
    else:
                                                        
                          
        lines.append(" & ".join([MISSING] * len(spec["header"])) + " \\\\")
    lines += [
        "\\bottomrule",
        "\\end{tabular}",
        f"\\caption{{{spec['caption']}}}",
        f"\\label{{{spec['label']}}}",
        "\\end{table}",
        "",
    ]
    return "\n".join(lines)


def missing_cells(body, width):
    """统计 TODO(实测) 单元格数，用于 --check 与收工报告。"""
    return sum(1 for row in body for value in row[:width] if value == MISSING)


def main():
    parser = argparse.ArgumentParser(description="CSV → LaTeX 表格片段")
    parser.add_argument("--csv", nargs="*", default=None,
                        help="指定 CSV 文件（默认读 result/csv/*.csv）")
    parser.add_argument("--check", action="store_true",
                        help="只报告缺格，不写文件")
    args = parser.parse_args()

    paths = [Path(p) for p in args.csv] if args.csv else sorted(CSV_DIR.glob("*.csv"))
    if not paths:
        print(f"no CSV under {CSV_DIR}", file=sys.stderr)
        rows = []
    else:
        rows = load_rows(paths)

    if not args.check:
        OUT_DIR.mkdir(parents=True, exist_ok=True)

    total_missing = 0
    for name, spec in TABLES.items():
        body = BUILDERS[name](rows)
        total_missing += missing_cells(body, len(spec["header"]))
        if args.check:
            print(f"{name}: {len(body)} 行，{missing_cells(body, len(spec['header']))} 格缺数据")
            continue
        out = OUT_DIR / f"{name}.tex"
        out.write_text(render(name, spec, body), encoding="utf-8")
        print(f"wrote {out.relative_to(ROOT)}")

    if args.check:
        print(f"total TODO(实测) cells: {total_missing}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
