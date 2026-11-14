#!/usr/bin/env python3
"""Host regressions for the real Redwood driver init functions and board parser.

No XNU SDK, firmware, phone or hardware access is used. The init entry points and
camera worker are compiled; kernel services and downstream driver work are stubs.
This checks dispatch and rejection ordering, not device support or a full build.

Examples:
  python tools/ace5/test-platform-guards.py --cc clang
  python tools/ace5/test-platform-guards.py --cc "C:/path with spaces/zig.exe" cc
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def extract_init(source: Path, name: str) -> str:
    """Copy the actual function, balancing braces outside C comments/literals."""
    text = source.read_text(encoding="utf-8")
    matches = list(re.finditer(r"^(?:static\s+)?void\s+" + re.escape(name) + r"\([^)]*\)\s*\{", text, re.M))
    if len(matches) != 1:
        raise ValueError(f"expected exactly one void {name} function in {source}")
    start = matches[0].start()
    brace = matches[0].end() - 1
    depth = 0
    tokens = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|[{}]', re.S)
    for token in tokens.finditer(text, brace):
        if token.group() == "{":
            depth += 1
        elif token.group() == "}":
            depth -= 1
            if depth == 0:
                line = text.count("\n", 0, start) + 1
                return f'#line {line} "{source.as_posix()}"\n{text[start:token.end()]}\n'
    raise ValueError(f"unterminated function {name} in {source}")


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        print("FAILED: " + subprocess.list2cmdline(command), file=sys.stderr)
        print(result.stdout + result.stderr, file=sys.stderr, end="")
        raise RuntimeError(f"command exited {result.returncode}")
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, file=sys.stderr, end="")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs=argparse.REMAINDER, default=["cc"],
                        help="compiler executable followed by arguments; must be last")
    args = parser.parse_args()
    if not args.cc:
        parser.error("--cc needs a compiler executable")
    root = Path(__file__).resolve().parents[2]
    fixtures = root / "tools/ace5/tests/platform-guards"
    # Keep generated code/executables outside the source checkout and delete them
    # after the run. Compilers can still maintain their own normal local caches.
    with tempfile.TemporaryDirectory(prefix="ace5-platform-guards-", dir=root.parent) as scratch:
        build = Path(scratch)
        for driver in ("wifi", "adsp", "camera", "speaker", "haptic", "torch"):
            source = root / f"xnu/files/bsd/dev/redwood_{driver}.c"
            (build / f"{driver}-init.inc").write_text(
                extract_init(source, f"redwood_{driver}_init"), encoding="utf-8")
        (build / "camera-thread.inc").write_text(extract_init(
            root / "xnu/files/bsd/dev/redwood_camera.c", "camera_thread"), encoding="utf-8")
        for test in ("platform", "wifi", "adsp", "camera", "camera-thread", "peripherals"):
            exe = build / (test + (".exe" if sys.platform == "win32" else ""))
            command = args.cc + ["-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra",
                                "-Wno-unused-function", "-Wno-unused-variable",
                                "-I", str(fixtures), "-I", str(build),
                                "-I", str(root / "xnu/files/bsd"),
                                str(fixtures / f"test-{test}.c"), "-o", str(exe)]
            run(command)
            run([str(exe)])
    print("PASS: all platform guard host tests (assertions enabled; no hardware exercised)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"platform guard tests: {error}", file=sys.stderr)
        raise SystemExit(1)
