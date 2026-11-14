#!/usr/bin/env python3
"""Check that Windows and Unix 7-Zip listings choose identical iOS inputs."""
from pathlib import Path
import subprocess
import sys
import tempfile

repo = Path(__file__).resolve().parents[2]
with tempfile.TemporaryDirectory() as tmp:
    root = Path(tmp)
    results = []
    for separator in ['/', '\\']:
        listing = root / 'listing.slt'
        records = []
        for path, mode, size, target in [
            ('usr/libexec/example', '-rwxr-xr-x', '12', ''),
            ('System/Library/Fonts/LanguageSupport/中文.ttf', '-rw-r--r--', '24', ''),
            ('private/var/mobile/Library', 'drwx------', '', ''),
            ('private/var/tmp', 'lrwxr-xr-x', '4', '/tmp'),
            ('etc', 'lrwxr-xr-x', '11', 'private/etc'),
        ]:
            records.append('Path = %s\nMode = %s\nSize = %s\nUser ID = 0\nGroup ID = 0\nSymbolic Link = %s\n\n' %
                           (path.replace('/', separator), mode, size, target))
        listing.write_text(''.join(records), encoding='utf-8')
        subprocess.run([sys.executable, str(repo / 'ios165/select-system.py'),
                        str(listing), str(root / 'files'), str(root / 'manifest')], check=True)
        skeleton = subprocess.check_output([sys.executable, '-X', 'utf8',
                         str(repo / 'ios165/mkvarskel.py'), str(listing)])
        results.append(((root / 'files').read_bytes(), (root / 'manifest').read_bytes(), skeleton))
    assert results[0] == results[1], 'separator changed selected inputs or metadata'
    assert b'usr/libexec/example\n' in results[0][0]
    assert '中文.ttf'.encode() in results[0][0]
    assert b'l\t/tmp\t/private/var/tmp' in results[0][2]
    assert b'd\t700\t0\t0\t/private/var/mobile/Library' in results[0][2]
print('iOS listing: Windows/Unix paths, UTF-8 names, owners and symlinks agree')
