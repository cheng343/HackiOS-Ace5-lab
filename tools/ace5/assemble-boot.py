#!/usr/bin/env python3
"""Assemble a private Ace 5 temporary boot image from cloud-built components.

No compiler, device command or network access. The live DTB remains local.
"""
import argparse
import gzip
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import tomllib

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--components', type=Path, required=True)
ap.add_argument('--dtb', type=Path, required=True)
ap.add_argument('--output', type=Path, required=True)
ap.add_argument('--stage', choices=('diagnostic', 'kernel'), required=True)
args = ap.parse_args()
p = args.components.resolve()
output = args.output.resolve()
if output.exists() or output.with_suffix('.json').exists():
    raise SystemExit('Refusing to overwrite an existing boot image or manifest')
meta = json.loads((p / 'components.json').read_text(encoding='utf-8'))
if (meta['device'] != 'PKG110 / giulia' or meta['mu_commit'] != '42fe563791e51bf5500c2e470ed294ca0c94b97c'
        or meta['fd_base'] != '0xa7100000' or meta['fd_size'] != 0x300000
        or meta['payload_base'] != '0xa8000000' or meta['device_dtb_included']):
    raise SystemExit('Unexpected UEFI component identity/layout')
def read(name):
    f = p / name
    data = f.read_bytes()
    entry = meta['files'][name]
    if len(data) != entry['bytes'] or hashlib.sha256(data).hexdigest() != entry['sha256']:
        raise SystemExit('Component hash mismatch: ' + name)
    return data
config = tomllib.loads(read('giulia.toml').decode())
if (config['uefi_fd']['base'], config['uefi_fd']['size'], config['boot_image']['header_version']) != (0xA7100000, 0x300000, 4):
    raise SystemExit('Unexpected giulia image configuration')
fd = read('SILICIUM_UEFI.fd')
shim = read('BootShim.bin')
if len(fd) != 0x300000 or not 0 < len(shim) <= 0x10000:
    raise SystemExit('Unexpected FD or BootShim size')
dtb = args.dtb.read_bytes()
if (len(dtb) < 40 or struct.unpack_from('>II', dtb) != (0xd00dfeed, len(dtb))
        or b'qcom,pineapple\0' not in dtb or b'giulia' not in dtb):
    raise SystemExit('The private DTB is not an expected giulia / pineapple FDT')
directory = 'giulia' if args.stage == 'diagnostic' else 'giulia-kernel'
build = json.loads(read(directory + '/loader-build.json'))
payload = read(directory + '/usb-payload.bin')
if (build['stage'] != args.stage or build['storage_writes']
        or build['kernel_execution'] != (args.stage == 'kernel')
        or hashlib.sha256(payload).hexdigest() != build['payload_sha256']
        or payload[:4] != b'XNUP' or len(payload) >= 0x03f00000):
    raise SystemExit('Loader payload does not match the requested stage')
read('mkbootimg.py')
read('ramdisk')
blob = shim + fd + payload
blob += b'\0' * (-len(blob) % 16)
output.parent.mkdir(parents=True, exist_ok=True)
kernel = output.parent / (output.stem + '.gz-dtb')
if kernel.exists():
    raise SystemExit('Refusing to overwrite an intermediate image')
kernel.write_bytes(gzip.compress(blob, mtime=0) + dtb)
subprocess.run([sys.executable, str(p / 'mkbootimg.py'), '--kernel', str(kernel),
                '--ramdisk', str(p / 'ramdisk'), '--header_version', '4',
                '--os_version', config['boot_image']['os_version'],
                '--os_patch_level', config['boot_image']['os_patch_level'],
                '-o', str(output)], check=True)
data = output.read_bytes()
if data[:8] != b'ANDROID!' or struct.unpack_from('<I', data, 40)[0] != 4:
    raise SystemExit('Generated image has an unexpected boot header')
manifest = dict(device='OnePlus Ace 5 PKG110 / giulia', stage=args.stage,
                kernel_execution=build['kernel_execution'], allowed_partition_write=['logdump'],
                loader_sha256=build['loader_sha256'], payload_sha256=build['payload_sha256'],
                mu_commit=meta['mu_commit'], source_commit=meta['source_commit'],
                fd_base='0xa7100000', fd_size=0x300000, payload_base='0xa8000000',
                dtb_sha256=hashlib.sha256(dtb).hexdigest(), image_bytes=len(data),
                sha256=hashlib.sha256(data).hexdigest(), header_version=4)
output.with_suffix('.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
print(json.dumps({'stage': args.stage, 'bytes': len(data), 'sha256': manifest['sha256']}, indent=2))
