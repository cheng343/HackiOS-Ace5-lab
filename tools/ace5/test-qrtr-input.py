#!/usr/bin/env python3
"""Cloud host regression for the real QRTR parser, using stubbed kernel services.

Extracts qrtr_input and its packet definitions directly from redwood_wifi.c.
No device, firmware or bus operations occur. Run in cloud CI with --cc clang-18.
"""
import argparse
import importlib.util
from pathlib import Path
import re
import subprocess
import sys
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', nargs=argparse.REMAINDER, default=['cc'])
    args = parser.parse_args()
    if not args.cc:
        parser.error('--cc needs a compiler')
    root = Path(__file__).resolve().parents[2]
    source = root / 'xnu/files/bsd/dev/redwood_wifi.c'
    text = source.read_text(encoding='utf-8')
    spec = importlib.util.spec_from_file_location('guards', Path(__file__).with_name('test-platform-guards.py'))
    guards = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(guards)
    constants = []
    for name in ('REDWOOD_QRTR_MAX', 'QRTR_CONTROL'):
        matches = re.findall(r'^#define ' + name + r'\s+.*$', text, re.M)
        if len(matches) != 1:
            raise ValueError('QRTR constant definition changed: ' + name)
        constants += matches
    enums = re.findall(r'^enum \{ QRTR_DATA.*?;', text, re.M)
    if len(enums) != 1:
        raise ValueError('QRTR message type definitions changed')
    first = text.index('struct qrtr_header {')
    last = text.index('struct redwood_qrtr_port {', first)
    with tempfile.TemporaryDirectory(prefix='ace5-qrtr-input-', dir=root.parent) as temporary:
        build = Path(temporary)
        (build / 'qrtr-types.inc').write_text('\n'.join(constants + enums) + '\n' + text[first:last], encoding='utf-8')
        (build / 'qrtr-input.inc').write_text(guards.extract_init(source, 'qrtr_input'), encoding='utf-8')
        exe = build / ('qrtr-input.exe' if sys.platform == 'win32' else 'qrtr-input')
        guards.run(args.cc + ['-std=c11', '-O1', '-UNDEBUG', '-g', '-Wall', '-Wextra',
                              '-fsanitize=address,undefined', '-I', str(build),
                              str(Path(__file__).with_name('test-qrtr-input.c')), '-o', str(exe)])
        guards.run([str(exe)])
    print('PASS: actual QRTR parser bounds and dispatch (host stubs; Wi-Fi hardware not exercised)')


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError) as error:
        raise SystemExit(str(error))
