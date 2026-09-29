#!/usr/bin/env python3
"""Fail if a `bash -lc '...'` block in release-linux.yml contains a stray single quote.

Those blocks are single-quoted shell strings. One single quote inside - an apostrophe in a comment
is enough - ends the string early, and everything after it is then run by the OUTER step script:
on the runner host instead of inside the container. Measured cost: the fedora toolchain probe ran
on ubuntu-latest and died with "gcc-15: command not found", spending a whole release run.

Usage: check-release-linux-quotes.py <.github/workflows/release-linux.yml>
"""

import sys

MARKER = "bash -lc '"


def stray_quotes(lines):
    bad = []
    index = 0
    while index < len(lines):
        line = lines[index]
        start = line.find(MARKER)
        if start >= 0:
            rest = line[start + len(MARKER):]
            # a block that opens and closes on the same line is fine
            if "'" not in rest:
                scan = index + 1
                while scan < len(lines) and not lines[scan].rstrip().endswith("'"):
                    if "'" in lines[scan]:
                        bad.append((scan + 1, lines[scan]))
                    scan += 1
        index += 1
    return bad


def main():
    if len(sys.argv) != 2:
        print("usage: check-release-linux-quotes.py <.github/workflows/release-linux.yml>")
        return 2
    path = sys.argv[1]
    with open(path, encoding="utf-8") as handle:
        bad = stray_quotes(handle.read().split("\n"))
    if bad:
        print(f"{path}: stray single quote(s) inside a bash -lc block:")
        for number, line in bad:
            print(f"  line {number}: {line.strip()}")
        return 1
    print(f"{path}: bash -lc blocks are free of stray quotes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
