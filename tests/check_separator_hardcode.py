#!/usr/bin/env python3
import re
import sys
from pathlib import Path

ALLOWED_STRING_LITERALS = ('"/.kvspace-meta"', '"://"')
FILESYSTEM_PATH_PREFIXES = ("/proc/", "/sys/", "/dev/", "/tmp/", "/var/",
                            "/usr/", "/etc/", "/home/")


def check_separator_hardcode(source_dir):
    errors = []
    for path in sorted(Path(source_dir).rglob("*")):
        if path.suffix not in (".c", ".h"):
            continue
        in_separator_table = False
        lines = path.read_text(encoding="utf-8").splitlines()
        for number, line in enumerate(lines, 1):
            stripped = line.strip()
            if "separator_table" in line and "=" in line:
                in_separator_table = "};" not in line
                continue
            if in_separator_table:
                if stripped.startswith("};"):
                    in_separator_table = False
                continue
            if stripped.startswith("#include"):
                continue
            checked = line
            for literal in ALLOWED_STRING_LITERALS:
                checked = checked.replace(literal, "")
            if "'/'" in checked or "'\\\\x2f'" in checked.lower():
                errors.append((path, number, "slash character literal", line))
            if re.search(r"0x[cC]2|0x[bB]7", checked):
                errors.append((path, number, "dot byte literal", line))
            if "·" in checked:
                errors.append((path, number, "dot character literal", line))
            if re.search(r"\\\\x2[fF]", checked):
                errors.append((path, number, "slash byte escape", line))
            if re.search(r"\\\\x[bB]7", checked):
                errors.append((path, number, "dot byte escape", line))
            for match in re.finditer(r'"(?:[^"\\\\]|\\\\.)*"', checked):
                literal = match.group(0)
                content = literal[1:-1]
                if "//" in content or content == "/":
                    errors.append((path, number, "slash in string literal", line))
                elif content.startswith("/") and not content.startswith(
                        FILESYSTEM_PATH_PREFIXES):
                    errors.append((path, number, "key-like string literal", line))
    return errors


def main(argv):
    if len(argv) != 2:
        print("usage: check_separator_hardcode.py <source-dir>", file=sys.stderr)
        return 2
    errors = check_separator_hardcode(argv[1])
    if not errors:
        print("separator hardcode: none")
        return 0
    for path, number, kind, text in errors:
        print(f"{path}:{number}: {kind}: {text.strip()}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
