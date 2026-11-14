#!/usr/bin/env python3
"""Run owner concurrency and real user-client/A750 input-boundary host tests.

Example: python tools/ace5/test-gpu-lifecycle.py --cc /path/to/zig cc
No kernel, device, firmware or Apple SDK is used.
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, capture_output=True, timeout=60)
    if result.returncode:
        raise RuntimeError(subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)


def extract_validation(root: Path, build: Path) -> None:
    source = root / "xnu/files/iokit/Kernel/RedwoodGPU.cpp"
    text = source.read_text(encoding="utf-8")
    types = []
    for name in ("msm_gem_submit_cmd", "msm_gem_submit_bo", "msm_gem_submit"):
        matches = list(re.finditer(r"^struct " + name + r" \{.*?^\};", text, re.M | re.S))
        if len(matches) != 1:
            raise ValueError(f"expected one {name} definition")
        types.append(matches[0].group())
    (build / "gpu-submit-types.inc").write_text("\n".join(types), encoding="utf-8")
    start_marker = "\tfor (uint32_t i = 0; i < request->nr_cmds; i++) {"
    end_marker = "\tif (gpu_log && request->fence != 0) {"
    if text.count(start_marker) != 1 or text.count(end_marker) != 1:
        raise ValueError("submission dispatch boundaries changed; review test extraction")
    start, end = text.index(start_marker), text.index(end_marker)
    if start >= end or "request->fence = redwood_gpu_submit" not in text[start:end]:
        raise ValueError("missing actual submission dispatch")
    line = text.count("\n", 0, start) + 1
    (build / "gpu-submit-validation.inc").write_text(
        f'#line {line} "{source.as_posix()}"\n' + text[start:end], encoding="utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs=argparse.REMAINDER, default=["cc"])
    args = parser.parse_args()
    if not args.cc:
        parser.error("--cc needs a compiler executable")
    root = Path(__file__).resolve().parents[2]
    with tempfile.TemporaryDirectory(prefix="ace5-gpu-lifecycle-", dir=root.parent) as scratch:
        build = Path(scratch)
        extract_validation(root, build)
        for test in ("owner", "validation"):
            executable = build / (test + (".exe" if sys.platform == "win32" else ""))
            command = args.cc + ["-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra",
                                "-Wno-unused-function", "-Wno-unused-variable",
                                "-I", str(build), str(root / f"tools/ace5/test-gpu-{test}.c"),
                                "-o", str(executable)]
            if sys.platform != "win32":
                command.append("-pthread")
            run(command)
            run([str(executable)])
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
