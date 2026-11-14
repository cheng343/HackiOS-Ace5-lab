#!/usr/bin/env python3
"""Exercise the actual EAGL IOSurface hash/upload/bind functions with host stubs.

Run in cloud CI: python3 tools/ace5/test-eagl-surface.py --cc clang-18
This does not test IOSurface locking semantics, the GL driver, or frame latency.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def section(text, start, end, path):
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError(f"EAGL test boundaries changed: {start!r}, {end!r}")
    first = text.index(start)
    last = text.index(end, first)
    return f'#line {text.count(chr(10), 0, first) + 1} "{path.as_posix()}"\n' + text[first:last]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs=argparse.REMAINDER, default=["cc"])
    args = parser.parse_args()
    if not args.cc:
        parser.error("--cc needs a compiler")
    root = Path(__file__).resolve().parents[2]
    source = root / "userland/src/eagl.c"
    text = source.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(prefix="ace5-eagl-surface-", dir=root.parent) as scratch:
        build = Path(scratch)
        (build / "image.inc").write_text(section(text, "struct image {", "struct eagl {", source), encoding="utf-8")
        functions = section(text, "static bool\nsurface_changed(",
                            "/* What has been drawn into a texture, into its IOSurface", source)
        functions += section(text, "static void\nmy_glBindTexture(", "static void\nmy_glClear(", source)
        (build / "functions.inc").write_text(functions, encoding="utf-8")
        exe = build / ("surface.exe" if sys.platform == "win32" else "surface")
        commands = [args.cc + ["-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra",
                               "-g", "-fsanitize=address,undefined", "-I", str(build),
                               str(root / "tools/ace5/tests/eagl-surface/test-surface.c"), "-o", str(exe)],
                    [str(exe)]]
        for command in commands:
            subprocess.run(command, check=True)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"EAGL surface test: {error}", file=sys.stderr)
        raise SystemExit(1)
