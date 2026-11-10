#!/usr/bin/env bash
# 下载 header-only ABI 依赖，安装到 /usr/include。本地与 CI 共用。
#
# 从 kvspace-c 迁入时改写过：原版遍历 deps.json（repo → tag）批量下载，包括
# **kvspace 自己**——并仓后 kvspace 不再是被依赖方，而本仓还剩两个真实外部依赖：
# blockmalloc 与 slotsboxmalloc。它们都是 header-only（本仓 TU 里
# `#define *_IMPLEMENTATION` 后编译实现体），只需要头。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# 版本钉在这里而不是 deps.json：只有两个依赖，为两个数字再引入一个 JSON
# 解析依赖（jq）不值得。改版本就改这两行。
#
# 这两个数字来自原 kvspace-c/deps.json 的 pin（blockmalloc v0.1.5 /
# slotsboxmalloc v0.1.7），不是仓库最新 tag——升级要单独决策。
BLOCKMALLOC_VERSION=v0.1.5
SLOTSBOXMALLOC_VERSION=v0.1.7

fetch_headers() {
  local repo=$1 ver=$2
  local tmp; tmp="$(mktemp -d)"
  gh release download "$ver" -R "array2d/$repo" \
     -p "${repo}-abi-*-linux-x86_64.tar.gz" -D "$tmp"
  tar xzf "$tmp"/*.tar.gz -C "$tmp" --strip-components=1
  if [ -d "$tmp/include" ]; then sudo cp -r "$tmp/include/"* /usr/include/; fi
  rm -rf "$tmp"
}

fetch_headers blockmalloc "$BLOCKMALLOC_VERSION"
fetch_headers slotsboxmalloc "$SLOTSBOXMALLOC_VERSION"
echo "✅ ABI deps → /usr/include（blockmalloc, slotsboxmalloc）"
