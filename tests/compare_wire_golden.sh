#!/bin/sh
# 比对两个 wire 金标准输出，逐字节。
#
#   tests/compare_wire_golden.sh <left-golden-binary> <right-golden-binary>
#
# 两者必须是**同一批操作**的产出（同一组用例、同一顺序），只有实现不同：
# kvspace 前端、kvspace-c、kvspace-durable 各跑一遍，输出必须完全一致。
#
# 只比字节，不解释字节——解释会引入第二套「什么算合法」的判断，而这里要验的
# 恰恰是两边对同一份 head 定义的理解一致。
set -eu

left_bin=$1
right_bin=$2

left_out=$("$left_bin")
right_out=$("$right_bin")

if [ "$left_out" = "$right_out" ]; then
    echo "wire golden: identical ($(printf '%s\n' "$left_out" | wc -l) cases)"
    exit 0
fi

echo "wire golden: MISMATCH" >&2
printf '%s\n' "$left_out" > /tmp/wire-golden-left.txt
printf '%s\n' "$right_out" > /tmp/wire-golden-right.txt
diff -u /tmp/wire-golden-left.txt /tmp/wire-golden-right.txt >&2 || true
exit 1
