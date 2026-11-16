#!/usr/bin/env python3
"""Host tests of loader USB download and per-run storage state.

Extracts the real command branches and run argument handling; USB, firmware and
memory-window services are stubs. No phone, firmware or full kernel build is used.
Usage: python tools/ace5/test-loader-state.py --cc clang
       python tools/ace5/test-loader-state.py --cc "C:/path to/zig.exe" cc
"""
import argparse
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def section(text: str, start: str, end: str, path: Path) -> str:
    if text.count(start) != 1 or text.count(end) != 1:
        raise ValueError(f"loader test boundaries changed: {start!r}, {end!r}")
    first = text.index(start)
    last = text.index(end, first)
    return f'#line {text.count(chr(10), 0, first) + 1} "{path.as_posix()}"\n' + text[first:last]


def run(command: list[str]) -> None:
    result = subprocess.run(command, text=True, capture_output=True)
    if result.returncode:
        raise RuntimeError(subprocess.list2cmdline(command) + "\n" + result.stdout + result.stderr)
    print(result.stdout, end="")
    print(result.stderr, end="", file=sys.stderr)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", nargs=argparse.REMAINDER, default=["cc"],
                        help="compiler and arguments; must be last")
    args = parser.parse_args()
    if not args.cc:
        parser.error("--cc needs a compiler")
    root = Path(__file__).resolve().parents[2]
    source = root / "xnuloader/loader.c"
    text = source.read_text(encoding="utf-8")
    fixtures = root / "tools/ace5/tests/loader-state"
    with tempfile.TemporaryDirectory(prefix="ace5-loader-state-", dir=root.parent) as scratch:
        build = Path(scratch)
        constants = []
        for name in ("PAYLOAD_MAX_SIZE", "RESIDENT_RD_OFFSET", "RESIDENT_RD_RESERVE", "USB_CHUNK"):
            matches = re.findall(r"^#define " + name + r"\s+.*$", text, re.M)
            if len(matches) != 1:
                raise ValueError(f"expected one definition of {name}")
            constants.append(matches[0])
        state = re.findall(r"^static uint64_t payload_available\s*=.*?;", text, re.M)
        if len(state) != 1:
            raise ValueError("expected one payload_available definition")
        (build / "constants.inc").write_text("\n".join(constants + state) + "\n", encoding="utf-8")
        command = section(text, '\t\t} else if (str_eq(cmd, "oem wlan-pdc-table")) {',
                          '\t\t} else if (starts_with(cmd, "upload")) {', source)
        (build / "commands.inc").write_text(
            'static void command(char *cmd) {\nif (0) {\n' + command + '\n}\n}\n', encoding="utf-8")
        run_state = section(text, '\tgamut_p3 = 0;',
                            '\tif (checkpoint == CHECKPOINT_SELFTEST) {\n\t\tkernel_mpidr = this_mpidr;', source)
        (build / "run-state.inc").write_text(run_state, encoding="utf-8")
        watchdog = section(text, 'static int\nobserver_watchdog_within_budget(',
                           '/*\n * Run the kernel on the core with that MPIDR', source)
        (build / "watchdog-budget.inc").write_text(watchdog, encoding="utf-8")
        quiet = section(text, 'static int\nobserver_firmware_quiet(', 'static int64_t\nrun_on_other_cpu(', source)
        (build / 'quiet-observer.inc').write_text(quiet, encoding='utf8')
        quiet_args = section(text, 'static int\nquiet_flag_present(', 'static int\nobserver_firmware_quiet(', source)
        (build / 'quiet-args.inc').write_text(quiet_args, encoding='utf8')
        quiet_enable=section(text,'    firmware_quiet_enabled = 0;',
                             '    if (firmware_quiet_enabled) {',source)
        (build / 'quiet-enable.inc').write_text(quiet_enable,encoding='utf8')
        prefix = section(text, 'static int\nstarts_with(', '/*\n * One command from the host.', source)
        gate = section(text, 'static int\ncrm_command_allowed(', 'static void\nfb_command(', source)
        (build / 'crm-command-gate.inc').write_text(prefix + gate, encoding='utf8')
        if text.index('if (!crm_command_allowed(cmd))') > text.index('if (busy && (starts_with(cmd, "oem wlan-pdc-table")'):
            raise ValueError('CRM retained-lifetime gate must precede all command dispatch')
        if '\twhile (status == 0) {\n        if (observer_firmware_quiet(started, budget, &last_pet)) continue;' not in text:
            raise ValueError('firmware quiet must gate every observer iteration before firmware services')
        for name in ("download", "run", "watchdog", "quiet"):
            exe = build / (name + (".exe" if sys.platform == "win32" else ""))
            run(args.cc + ["-std=c11", "-O1", "-UNDEBUG", "-Wall", "-Wextra",
                           "-I", str(build), "-I", str(root/'xnu/files/bsd/sys'),
                           str(fixtures / f"test-{name}.c"), "-o", str(exe)])
            run([str(exe)])
    print("PASS: loader download, per-run storage state and watchdog budget (host stubs; no hardware)")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, RuntimeError) as error:
        print(f"loader state test: {error}", file=sys.stderr)
        raise SystemExit(1)
