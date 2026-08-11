#!/usr/bin/env bash
# PreCommit: auto format C/C++ source files with clang-format
# Triggered before any git commit in kvspace-c

cd "$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0

# Only run if clang-format exists
command -v clang-format >/dev/null 2>&1 || exit 0

# Find all .c/.h/.cpp/.hpp files and format them
FILES=$(find . -not -path './build/*' -not -path './target/*' -name '*.c' -o -name '*.h' -o -name '*.cpp' -o -name '*.hpp')
if [ -z "$FILES" ]; then
  exit 0
fi

echo "Formatting C/C++ files..."
for f in $FILES; do
  if [ -f "$f" ]; then
    clang-format -i "$f"
    echo "  formatted: $f"
  fi
done
echo "done."
