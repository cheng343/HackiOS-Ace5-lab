#!/usr/bin/env python3
"""Cloud-only host regressions for real NFG bus and extracted board lifecycle.

Usage: python3 tools/ace5/test-giulia-battery.py --cc clang-18
These mocks cannot validate pin wiring, electrical ownership, real responses or iOS UI.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def section(text, start, end, source):
    if text.count(start) != 1 or (end and text.count(end) != 1):
        raise ValueError(f"battery test source boundaries changed: {start!r}, {end!r}")
    first = text.index(start)
    last = text.index(end, first) if end else len(text)
    return f'#line {text.count(chr(10), 0, first) + 1} "{source.as_posix()}"\n' + text[first:last]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', nargs=argparse.REMAINDER, default=['cc'])
    args = ap.parse_args()
    if not args.cc:
        ap.error('--cc requires a compiler')
    root = Path(__file__).resolve().parents[2]
    source = root / 'xnu/files/bsd/dev/giulia_battery.c'
    text = source.read_text(encoding='utf-8')
    with tempfile.TemporaryDirectory(prefix='ace5-battery-', dir=root.parent) as scratch:
        build = Path(scratch)
        for name, start, end in (
            ('state', 'enum { GB_IDLE', 'static uint32_t gb_mmio_read'),
            ('gpio', 'static bool gb_running(', 'static void gb_thread('),
            ('lifecycle', 'void giulia_battery_stop(', None),
        ):
            (build / f'{name}.inc').write_text(section(text, start, end, source), encoding='utf-8')
        fixtures = [root / 'tools/ace5/test-giulia-battery.c',
                    root / 'tools/ace5/tests/giulia-battery/test-lifecycle.c']
        for fixture in fixtures:
            exe = build / (fixture.stem + ('.exe' if sys.platform == 'win32' else ''))
            subprocess.run(args.cc + ['-std=c11', '-O1', '-UNDEBUG', '-Wall', '-Wextra', '-g',
                                     '-fsanitize=address,undefined', '-I', str(build), str(fixture), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)
    return 0


if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f'battery regression: {error}', file=sys.stderr)
        raise SystemExit(1)
