#!/usr/bin/env python3
"""Cloud host tests of the real SM8650 SNPS MSI core/adapter; no device or SDK.

Run in CI: python tools/ace5/test-pcie-msi.py --cc cc
Local source-only inspection: python tools/ace5/test-pcie-msi.py --check-only
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
    print(result.stderr, end="", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--cc", nargs=argparse.REMAINDER, default=["cc"])
    args = parser.parse_args()
    if not args.cc:
        parser.error("--cc needs a compiler executable")
    root = Path(__file__).resolve().parents[2]
    include = root / "xnu/files/bsd/dev"
    source = include / "giulia_pcie_msi.c"
    header = include / "giulia_pcie_msi.h"
    test = root / "tools/ace5/test-pcie-msi.c"
    wrapper_test = root / "tools/ace5/test-giulia-pcie-msi-xnu.c"
    for path in (source, header, test, wrapper_test,
                 include / "giulia_pcie_msi_xnu.c", include / "giulia_pcie_msi_xnu.h"):
        if not path.is_file():
            raise ValueError(f"missing source: {path}")
        path.read_text(encoding="utf-8")
    if args.check_only:
        print("SM8650 MSI source inputs present; C tests NOT compiled or executed")
        return 0
    with tempfile.TemporaryDirectory(prefix="ace5-pcie-msi-", dir=root.parent) as scratch:
        # The adapter fixture includes its real .c with fake core/GIC services;
        # do not link the real core into that separate lifecycle executable.
        for name, sources in (("core", [source, test]), ("xnu", [wrapper_test])):
            exe = Path(scratch) / (name + (".exe" if sys.platform == "win32" else ""))
            run(args.cc + ["-std=c11", "-O1", "-g", "-fsanitize=address,undefined",
                           "-UNDEBUG", "-Wall", "-Wextra", "-Werror",
                           "-I", str(include)] + [str(path) for path in sources] +
                ["-o", str(exe)])
            run([str(exe)])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
