#!/usr/bin/env bash
# 发布包 ABI/R 面检查器（只读）。
#
# 用法：check_release_abi.sh <解包后的发布目录>
#   该目录须含 lib/ 与 include/kvspace/（cmake --install 产出的布局）。
#
# 为什么需要它：链接测试（tests/test_export_surface.c）挡「头里声明了、库里没有」，
# 它看不见「库里多出来了」——kvspaceShm* 泄漏、旧符号残留都属后者。两个关口
# 缺一个都会留缺口，所以这条检查器补的是**打包后的实物**。
#
# 为什么不在这里写第二份符号清单：清单的唯一来源是
# `tests/test_export_surface.c` 的地址表。两份清单必然漂移——`const.h` 那次事故
# 就是「一份契约没人对账」的形态，这里不重犯。
#
# 四组断言：
#   1. lib/ 恰好三个 libkvspace.so 系列文件；
#   2. .so 的 T 符号恰好 16 个，且与 test_export_surface.c 的表**逐个同名**；
#   3. grep -c '^kvspaceShm' == 0（内部符号不得泄漏）；
#   4. include/kvspace/ 恰好三个头，且一个 C 文件 include 全部三个能编译通过。
#
# 失败时打印**具体差集**（多了谁、少了谁），不只报个数。
# 只读：不修改被检查目录。

set -u

RED='\033[0;31m'; GREEN='\033[0;32m'; YELLOW='\033[1;33m'; NC='\033[0m'
say()  { printf '%s\n' "$*"; }
ok()   { printf "${GREEN}PASS${NC}  %s\n" "$*"; }
bad()  { printf "${RED}FAIL${NC}  %s\n" "$*"; }
warn() { printf "${YELLOW}WARN${NC}  %s\n" "$*"; }

if [ $# -ne 1 ]; then
    say "usage: $0 <unpacked-release-dir>" >&2
    exit 2
fi

RELEASE_DIR=$1
if [ ! -d "$RELEASE_DIR" ]; then
    bad "not a directory: $RELEASE_DIR"
    exit 2
fi

LIB_DIR="$RELEASE_DIR/lib"
INC_DIR="$RELEASE_DIR/include/kvspace"
SONAME_FILE="$LIB_DIR/libkvspace.so.1.0.0"

# ── 0. 布局闸：先分清「目录给错了」与「内容不对」 ────────────────────
#
# `cmake --install --prefix X` 把库装到 `X/lib/kvspace/`（CMakeLists 的
# `LIBRARY DESTINATION .../kvspace`），而 release tarball 把它们平铺在 `lib/`。
# 两种布局里**只有一种是本检查器的对象**。
#
# 为什么不在下面各步各自兜底、也不让检查器两种都接受：那会变成「两个入口」——
# 一个检查器同时认两种布局，就意味着它没有确定的判据，将来第三种布局出现时没人
# 知道该不该加。这里显式只认发布包布局，别的直接退出去并说清该怎么转。
#
# 判据：**错误信息必须让人一眼分清「布局不对」与「内容不对」**。之前有过一次
# 因为检查器硬编码路径而误判「handler 缺失」的事故，排查花了一轮。
#
# 判据的落点：**看库文件实际在哪，不看目录在不在。**
#
# 第一版写成 `[ ! -d "$LIB_DIR" ] && ...`，实测这个闸**从不触发**——因为
# `cmake --install` 的布局里 `lib/` 是存在的（它是 `lib/kvspace/` 的父目录），
# 于是条件恒假，流程直接掉进第 1 组断言报「缺少 3 个文件」。
# **目录存在不代表内容在正确位置。**
#
# 但只判「`lib/kvspace/` 存在」又会走向另一个错：发布包里若意外多出这个空子目录，
# 就被误判成 install 布局。所以**两个条件都要成立**才归为 install 布局：
#   1. `lib/` 平铺处**没有** libkvspace 系列（否则它就是发布包）；
#   2. `lib/kvspace/` 里**有**。
if ! ls "$LIB_DIR"/libkvspace.so* >/dev/null 2>&1 &&
   ls "$RELEASE_DIR"/lib/kvspace/libkvspace.so* >/dev/null 2>&1; then
    bad "$RELEASE_DIR 不是发布包布局（库在 lib/kvspace/ 子目录里，不在 lib/ 平铺）"
    say "      这看起来是 **cmake --install 的前缀目录**，不是 release tarball 解包后的目录。"
    say "      本检查器只验发布包布局：lib/libkvspace.so* + include/kvspace/。"
    say "      要验安装布局，先把它摊平再跑："
    say "        cp -P '$RELEASE_DIR/lib/kvspace/libkvspace.so'* '$RELEASE_DIR/lib/'"
    exit 2
fi

failures=0

# ── 1. lib/ 恰好三个 libkvspace.so 系列 ──────────────────────────────
say "── 1. lib/ 文件集合 ──"
if [ ! -d "$LIB_DIR" ]; then
    bad "missing directory: $LIB_DIR"
    failures=$((failures + 1))
else
    # 只数 libkvspace.so* 这一族（发布包里还有 libkvlanglayout.so 等，不属本检查）
    mapfile -t found < <(cd "$LIB_DIR" && ls -1 libkvspace.so* 2>/dev/null | sort)
    expected=(libkvspace.so libkvspace.so.1 libkvspace.so.1.0.0)
    missing=(); extra=()
    for e in "${expected[@]}"; do
        printf '%s\n' "${found[@]}" | grep -qx -- "$e" || missing+=("$e")
    done
    for f in "${found[@]}"; do
        printf '%s\n' "${expected[@]}" | grep -qx -- "$f" || extra+=("$f")
    done
    if [ ${#missing[@]} -eq 0 ] && [ ${#extra[@]} -eq 0 ]; then
        ok "lib/ 恰好 3 个：${expected[*]}"
    else
        bad "lib/ 文件集合不符（期望 3 个）"
        [ ${#missing[@]} -gt 0 ] && say "      缺少（${#missing[@]}）：${missing[*]}"
        [ ${#extra[@]} -gt 0 ]   && say "      多出（${#extra[@]}）：${extra[*]}"
        failures=$((failures + 1))
    fi
fi

# ── 2. 导出 T 符号恰好 16，且与 test_export_surface.c 的表同名 ────────
say ""
say "── 2. 导出符号集合 ──"
# 清单唯一来源：tests/test_export_surface.c。解析 `(const void *)&symbol,` 行。
SOURCE_TABLE="$(dirname "$0")/test_export_surface.c"
if [ ! -f "$SOURCE_TABLE" ]; then
    bad "找不到清单来源：$SOURCE_TABLE"
    failures=$((failures + 1))
    SOURCE_TABLE=""
fi

if [ ! -f "$SONAME_FILE" ]; then
    bad "找不到 $SONAME_FILE"
    failures=$((failures + 1))
elif [ -z "$SOURCE_TABLE" ]; then
    : # 上面已计失败
else
    # 期望集合：从 .c 抽名字（去掉 (const void *)& 与行尾逗号）
    mapfile -t expected_syms < <(
        grep -oE '\(const void \*\)&kvspace[A-Za-z0-9_]+' "$SOURCE_TABLE" |
        sed 's/.*&//' | sort -u
    )
    # 实际集合：.so 的 T 符号里以 kvspace 开头的
    mapfile -t actual_syms < <(
        nm -D --defined-only "$SONAME_FILE" 2>/dev/null |
        awk '$2=="T"{print $3}' | grep '^kvspace' | sort -u
    )

    if [ ${#expected_syms[@]} -eq 0 ]; then
        bad "从 $SOURCE_TABLE 没解析出任何符号（格式变了？）"
        failures=$((failures + 1))
    else
        missing_syms=(); extra_syms=()
        while IFS= read -r s; do
            [ -z "$s" ] && continue
            printf '%s\n' "${actual_syms[@]}" | grep -qx -- "$s" || missing_syms+=("$s")
        done < <(printf '%s\n' "${expected_syms[@]}")
        while IFS= read -r s; do
            [ -z "$s" ] && continue
            printf '%s\n' "${expected_syms[@]}" | grep -qx -- "$s" || extra_syms+=("$s")
        done < <(printf '%s\n' "${actual_syms[@]}")

        if [ ${#missing_syms[@]} -eq 0 ] && [ ${#extra_syms[@]} -eq 0 ] &&
           [ ${#actual_syms[@]} -eq 16 ]; then
            ok "导出 T 符号恰好 16 个，与 test_export_surface.c 的表逐个同名"
        else
            bad "导出符号集合不符（实际 ${#actual_syms[@]} 个，期望 16）"
            [ ${#missing_syms[@]} -gt 0 ] && say "      库里缺少（${#missing_syms[@]}）：${missing_syms[*]}"
            [ ${#extra_syms[@]} -gt 0 ]   && say "      库里多出（${#extra_syms[@]}）：${extra_syms[*]}"
            failures=$((failures + 1))
        fi
    fi

    # ── 3. kvspaceShm* 泄漏必须为 0 ──────────────────────────────────
    say ""
    say "── 3. 内部符号泄漏 ──"
    leak_count=$(nm -D --defined-only "$SONAME_FILE" 2>/dev/null |
                 awk '$2=="T"{print $3}' | grep -c '^kvspaceShm')
    if [ "$leak_count" -eq 0 ]; then
        ok "kvspaceShm* 泄漏 0"
    else
        bad "kvspaceShm* 泄漏 $leak_count 个（内部符号不得导出）"
        nm -D --defined-only "$SONAME_FILE" 2>/dev/null |
            awk '$2=="T"{print $3}' | grep '^kvspaceShm' | sed 's/^/      /'
        failures=$((failures + 1))
    fi
fi

# ── 4. include/kvspace/ 恰好三个头 + 三头齐 include 能编译 ────────────
say ""
say "── 4. 头文件 ──"
if [ ! -d "$INC_DIR" ]; then
    bad "missing directory: $INC_DIR"
    failures=$((failures + 1))
else
    mapfile -t heads < <(cd "$INC_DIR" && ls -1 *.h 2>/dev/null | sort)
    expected_heads=(const.h kvspace.h kvspace_xvalue.h)
    hmissing=(); hextra=()
    for e in "${expected_heads[@]}"; do
        printf '%s\n' "${heads[@]}" | grep -qx -- "$e" || hmissing+=("$e")
    done
    for f in "${heads[@]}"; do
        printf '%s\n' "${expected_heads[@]}" | grep -qx -- "$f" || hextra+=("$f")
    done
    if [ ${#hmissing[@]} -eq 0 ] && [ ${#hextra[@]} -eq 0 ]; then
        ok "include/kvspace/ 恰好 3 个头：${expected_heads[*]}"
    else
        bad "include/kvspace/ 头文件集合不符"
        [ ${#hmissing[@]} -gt 0 ] && say "      缺少：${hmissing[*]}"
        [ ${#hextra[@]} -gt 0 ]   && say "      多出：${hextra[*]}"
        failures=$((failures + 1))
    fi

    # 编译探针：三个头全 include，只查语法（-fsyntax-only 不产生产物）
    probe=$(mktemp /tmp/release_abi_probe_XXXXXX.c)
    cat > "$probe" <<'PROBE'
#include <kvspace/const.h>
#include <kvspace/kvspace.h>
#include <kvspace/kvspace_xvalue.h>
int main(void) { return 0; }
PROBE
    if cc -std=c11 -Wall -Wextra -Werror -fsyntax-only \
          -I "$RELEASE_DIR/include" "$probe" 2>/tmp/release_abi_probe.err; then
        ok "三头齐 include 编译通过"
    else
        bad "三头齐 include 编译失败"
        sed 's/^/      /' /tmp/release_abi_probe.err | head -10
        failures=$((failures + 1))
    fi
    rm -f "$probe" /tmp/release_abi_probe.err

    # ── 4b. 头里声明的函数必须真的能链接 ─────────────────────────────
    #
    # 这一条抓的正是 v0.3.32 的事故：`const.h` 声明 `kvspaceConst`，
    # 而库只导出 `kvspaceConstStorage`。**符号集合检查抓不到它**——库的 16 个
    # 符号是对的，错的是头。只查「头 vs 库」这一对才知道。
    #
    # 形态：从头里抽出 `const char *kvspace*(...)` 这类**函数声明**（跳过
    # `static inline` —— 那些定义在头里，本来就不该出现在 .so），逐个取地址
    # 编译。缺任何一个都是「能编过、一链接就找不到符号」。
    if [ -f "$SONAME_FILE" ]; then
        decl_probe=$(mktemp /tmp/release_abi_decl_XXXXXX.c)
        {
            echo '#include <kvspace/const.h>'
            echo '#include <kvspace/kvspace.h>'
            echo '#include <kvspace/kvspace_xvalue.h>'
            echo 'static const void *const probe_table[] = {'
            # 抽所有 `kvspace*(` 形式的标识符（含 inline 的也无妨：inline 函数
            # 在头里有定义，取地址会解析到本地副本，不会误报缺失）
            grep -hoE '\bkvspace[A-Za-z0-9_]+ *\(' "$INC_DIR"/*.h |
                sed -E 's/ *\($//' | sort -u |
                while IFS= read -r sym; do
                    echo "    (const void *)&$sym,"
                done
            echo '};'
            echo 'int main(void){ (void)probe_table; return 0; }'
        } > "$decl_probe"

        if cc -std=c11 -fsyntax-only -I "$RELEASE_DIR/include" "$decl_probe" \
                2>/tmp/release_abi_decl.err; then
            # 语法过了还不够：必须**链接**一次，才知道符号是否真的在 .so 里
            decl_obj=$(mktemp /tmp/release_abi_decl_XXXXXX.o)
            if cc -std=c11 -c -I "$RELEASE_DIR/include" "$decl_probe" \
                    -o "$decl_obj" 2>/dev/null &&
               cc "$decl_obj" -o /dev/null -L "$LIB_DIR" -lkvspace \
                    -Wl,-rpath,"$LIB_DIR" 2>/tmp/release_abi_link.err; then
                ok "头里声明的函数全部能在 .so 里链接到"
            else
                bad "头里有声明、库里链接不到（能编过、一链接就失败）"
                grep -oE "undefined reference to \`[^']+'" /tmp/release_abi_link.err |
                    sort -u | sed 's/^/      /' | head -10
                failures=$((failures + 1))
            fi
            rm -f "$decl_obj" /tmp/release_abi_link.err
        else
            bad "按头文件声明构造的探针无法编译"
            sed 's/^/      /' /tmp/release_abi_decl.err | head -10
            failures=$((failures + 1))
        fi
        rm -f "$decl_probe" /tmp/release_abi_decl.err
    fi
fi

# ── 汇总 ────────────────────────────────────────────────────────────
say ""
if [ "$failures" -eq 0 ]; then
    printf "${GREEN}══ release ABI 检查通过 ══${NC}\n"
    exit 0
fi
printf "${RED}══ release ABI 检查失败：%d 组 ══${NC}\n" "$failures"
exit 1
