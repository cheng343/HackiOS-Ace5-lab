#!/usr/bin/env python3
"""Cloud-only tests of the actual SM8650 PCIe power/PHY/link source.

MMIO and clock services are mocked; success cannot establish hardware support.
Use --check-only locally to inspect inputs without compiling C.
"""
import argparse
from pathlib import Path
import subprocess
import sys
import tempfile


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--check-only', action='store_true')
    ap.add_argument('--cc', nargs=argparse.REMAINDER, default=['cc'])
    args = ap.parse_args()
    if not args.cc:
        ap.error('--cc needs a compiler')
    root = Path(__file__).resolve().parents[2]
    drivers = root/'xnu/files/bsd/dev'
    cases = [(drivers/f'giulia_pcie_{name}.c', root/f'tools/ace5/test-pcie-{name}.c')
             for name in ('power', 'phy', 'link')]
    for case in cases:
        for path in case:
            path.read_text(encoding='utf-8')
    if args.check_only:
        print('PCIe bringup source inputs present; C tests NOT compiled or executed')
        return
    with tempfile.TemporaryDirectory(prefix='ace5-pcie-bringup-', dir=root.parent) as temporary:
        for source, test in cases:
            exe = Path(temporary)/(test.stem + ('.exe' if sys.platform == 'win32' else ''))
            subprocess.run(args.cc + ['-std=c11', '-O1', '-g', '-UNDEBUG', '-Wall', '-Wextra',
                '-Werror', '-fsanitize=address,undefined', '-I', str(drivers),
                str(source), str(test), '-o', str(exe)], check=True, timeout=90)
            subprocess.run([str(exe)], check=True, timeout=30)


if __name__ == '__main__':
    try:
        main()
    except (OSError, subprocess.SubprocessError) as error:
        raise SystemExit(str(error))
