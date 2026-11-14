#!/usr/bin/env python3
"""Cloud host mock tests of the real SM8650 PCIe link/config core.

Use --check-only for local source presence checks; it never invokes a compiler.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(subprocess.list2cmdline(command) + "\n" +
                           result.stdout + result.stderr)
    print(result.stdout, end="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--cc", nargs=argparse.REMAINDER, default=["cc"])
    args = parser.parse_args()
    if not args.cc:
        parser.error("--cc needs a compiler executable")
    root = Path(__file__).resolve().parents[2]
    include = root / "xnu/files/bsd/dev"
    source = include / "giulia_pcie_link.c"
    header = include / "giulia_pcie_link.h"
    test = root / "tools/ace5/test-pcie-link.c"
    for path in (source, header, test):
        path.read_text(encoding="utf-8")
    if args.check_only:
        print("PCIe link source inputs present; C tests NOT compiled or executed")
        return 0
    with tempfile.TemporaryDirectory(prefix="ace5-pcie-link-", dir=root.parent) as tmp:
        exe = Path(tmp) / ("link.exe" if sys.platform == "win32" else "link")
        run(args.cc + ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                       "-UNDEBUG", "-Wall", "-Wextra", "-Werror", "-I", str(include),
                       str(source), str(test), "-o", str(exe)])
        run([str(exe)])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
