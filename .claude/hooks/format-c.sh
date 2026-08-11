#!/usr/bin/env bash
# PreCommit: auto format C/C++ source files with clang-format
# Triggered before any git commit in kvspace-c

cd "$(git rev-parse --show-toplevel 2>/dev/null)" || exit 0

# Only run if clang-format exists
command -v clang-format >/dev/null 2>&1 || exit 0

# Find staged .c/.h/.cpp/.hpp files and format them
STAGED=$(git diff --cached --name-only --diff-filter=ACM | grep -E '\.(c|h|cpp|hpp)$')
if [ -z "$STAGED" ]; then
  exit 0
fi

echo "Formatting staged C/C++ files..."
for f in $STAGED; do
  if [ -f "$f" ]; then
    clang-format -i "$f"
    git add "$f"
    echo "  formatted: $f"
  fi
done
echo "done."
