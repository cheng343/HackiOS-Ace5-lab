#!/usr/bin/env python3
"""Record cloud runner capacity and only the whitelisted build outputs."""
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
OUT = ROOT / 'out/ci'
OUT.mkdir(parents=True, exist_ok=True)

def save(path, value):
    path.write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

if sys.argv[1] == 'inventory':
    disk = shutil.disk_usage(ROOT)
    record = dict(stage=sys.argv[2], time=datetime.now(timezone.utc).isoformat(),
                  architecture=platform.machine(), cpu_count=os.cpu_count(),
                  disk_bytes=dict(total=disk.total, used=disk.used, free=disk.free),
                  memory_bytes=os.sysconf('SC_PAGE_SIZE') * os.sysconf('SC_PHYS_PAGES'))
    save(OUT / f'capacity-{sys.argv[2]}.json', record)
    print(json.dumps(record, indent=2))
elif sys.argv[1] == 'manifest':
    files = []
    for name in ('mach.ace5', 'giulia/loader.efi', 'giulia/usb-payload.bin',
                 'giulia/loader-build.json', 'giulia-kernel/loader.efi',
                 'giulia-kernel/usb-payload.bin', 'giulia-kernel/loader-build.json'):
        path = OUT / 'artifacts' / name
        if path.is_file():
            files.append(dict(path=name, bytes=path.stat().st_size,
                              sha256=hashlib.sha256(path.read_bytes()).hexdigest()))
    save(OUT / 'build-manifest.json', dict(
        source_commit=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip(),
        run_id=os.environ.get('GITHUB_RUN_ID'), run_attempt=os.environ.get('GITHUB_RUN_ATTEMPT'),
        xnu_base='c54f35ca767986246321eb901baf8f5ff7923f6a', llvm=18,
        sdk_commit='0222fd5413cf4b9af096f37b4621afa2688572f7',
        kernel_built=any(f['path'] == 'mach.ace5' for f in files),
        device_test=False, phone_operations=False, files=files))
else:
    raise SystemExit('Usage: ci-record.py inventory STAGE | manifest')
