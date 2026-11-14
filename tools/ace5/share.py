#!/usr/bin/env python3
"""Portable Ace 5 package: verify, prepare with own DTB/backup, launch, return.

Preparation only reads Android. Startup uses the guarded temporary boot flow.
The package contains no donor phone identity, DTB or boot-chain backup.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import struct
import subprocess
import sys
import time


MODEL = 'PKG110'
BUILD = 'PKG110_17.0.0.155(CN01)'
PARTITIONS = {f'{base}_{slot}' for base in (
    'boot', 'init_boot', 'vendor_boot', 'dtbo', 'vbmeta', 'vbmeta_system',
    'vbmeta_vendor', 'recovery', 'xbl', 'xbl_config', 'uefi') for slot in ('a', 'b')}


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def inside(root, relative):
    """Reject ambiguous archive paths as well as escaping symlinks."""
    if not isinstance(relative, str) or not relative or '\\' in relative or ':' in relative:
        raise RuntimeError('Invalid package path')
    parts = relative.split('/')
    if any(part in ('', '.', '..') for part in parts) or PurePosixPath(relative).is_absolute():
        raise RuntimeError('Unsafe package path: ' + relative)
    path = root.joinpath(*parts)
    if not path.resolve().is_relative_to(root.resolve()):
        raise RuntimeError('Package path escapes its directory: ' + relative)
    return path


def checked_file(path, entry):
    if (not isinstance(entry, dict) or not isinstance(entry.get('bytes'), int)
            or entry['bytes'] < 0 or not isinstance(entry.get('sha256'), str)
            or not re.fullmatch('[0-9a-f]{64}', entry['sha256'])):
        raise RuntimeError('Malformed checksum entry: ' + path.name)
    if not path.is_file() or path.stat().st_size != entry['bytes'] or digest(path) != entry['sha256']:
        raise RuntimeError('Missing or modified file: ' + str(path))


def fdt_properties(data):
    """Parse the live FDT without invoking a device-tree compiler."""
    if not 40 <= len(data) <= 8 * 1024 * 1024:
        raise RuntimeError('Unexpected live DTB size')
    magic, total, off, strings_off, reserve, version, last, cpu, strings_len, size = struct.unpack_from('>10I', data)
    if (magic != 0xd00dfeed or total != len(data) or version < 17 or last > 17
            or off < 40 or strings_off < 40 or off + size > total or strings_off + strings_len > total):
        raise RuntimeError('Invalid live DTB header')
    strings = data[strings_off:strings_off + strings_len]
    stop = off + size
    stack = []
    nodes = {}
    while off + 4 <= stop:
        token = struct.unpack_from('>I', data, off)[0]
        off += 4
        if token == 1:
            end = data.find(b'\0', off, stop)
            if end < 0:
                raise RuntimeError('Unterminated DTB node')
            name = data[off:end].decode('ascii')
            if '/' in name or (not stack and name):
                raise RuntimeError('Invalid DTB node name')
            stack.append(name)
            path = '/'.join(stack) or '/'
            if path in nodes:
                raise RuntimeError('Duplicate DTB node')
            nodes[path] = {}
            off = (end + 4) & ~3
        elif token == 2:
            if not stack:
                raise RuntimeError('Unbalanced DTB node')
            stack.pop()
        elif token == 3:
            if not stack or off + 8 > stop:
                raise RuntimeError('Invalid DTB property')
            length, string_off = struct.unpack_from('>II', data, off)
            off += 8
            end = strings.find(b'\0', string_off)
            if string_off >= len(strings) or end < 0 or off + length > stop:
                raise RuntimeError('Out-of-bounds DTB property')
            name = strings[string_off:end].decode('ascii')
            props = nodes['/'.join(stack) or '/']
            if name in props:
                raise RuntimeError('Duplicate DTB property')
            props[name] = data[off:off + length]
            off = (off + length + 3) & ~3
        elif token == 4:
            pass
        elif token == 9:
            if stack:
                raise RuntimeError('Unbalanced DTB end')
            return nodes
        else:
            raise RuntimeError('Unknown DTB token')
    raise RuntimeError('Missing DTB end')


class Package:
    def __init__(self, root):
        self.root = root.resolve()
        self.source = self.root / 'source/HackiOS-Ace5'
        self.state_path = self.root / 'private/prepared.json'
        self.adb = self.root / 'tools/platform-tools/adb.exe'
        self.env = dict(os.environ)
        self.env['PATH'] = str(self.adb.parent) + os.pathsep + self.env.get('PATH', '')
        self.env['PYTHONUTF8'] = '1'

    def execute(self, command, timeout=60, capture=False):
        result = subprocess.run([str(item) for item in command], env=self.env,
                                timeout=timeout, check=False,
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
        if result.returncode:
            message = (result.stderr or b'').decode('utf-8', errors='replace')[-1600:]
            raise RuntimeError(f'Command failed ({Path(str(command[0])).name}): {message}')
        return result.stdout if capture else b''

    def verify(self):
        manifest = json.loads((self.root / 'manifest.json').read_text(encoding='utf-8'))
        entries = manifest.get('files')
        required = {
            'compatibility.json', 'kernels/mach.ace5', 'images/system.apfs', 'images/root-display.apfs',
            'components/uefi/components.json', 'tools/platform-tools/adb.exe',
            'tools/platform-tools/fastboot.exe', 'source/HackiOS-Ace5/tools/ace5/phone.py',
            'source/HackiOS-Ace5/tools/ace5/backup.py', 'source/HackiOS-Ace5/tools/ace5/assemble-boot.py'}
        if not isinstance(entries, dict) or not required.issubset(entries):
            raise RuntimeError('Incomplete package manifest')
        for relative, entry in entries.items():
            checked_file(inside(self.root, relative), entry)
        print(f'Package verified: {len(entries)} files.', flush=True)

    def android(self):
        response = self.execute([self.adb, 'devices'], capture=True).decode('utf-8', errors='replace')
        devices = [line.split() for line in response.splitlines()[1:] if line.strip()]
        if len(devices) != 1 or len(devices[0]) != 2 or devices[0][1] != 'device':
            raise RuntimeError('Connect exactly one Android phone and authorize USB debugging.')
        serial = devices[0][0]
        if not re.fullmatch('[A-Za-z0-9_.:-]+', serial):
            raise RuntimeError('Unexpected device serial format')
        def text(*command):
            return self.execute([self.adb, '-s', serial, *command], capture=True).decode('utf-8', errors='strict').strip()
        model = text('shell', 'getprop', 'ro.product.model')
        build = text('shell', 'getprop', 'ro.build.display.id')
        if model != MODEL or build != BUILD:
            raise RuntimeError(f'This experimental package requires {MODEL} / {BUILD}; got {model} / {build}.')
        if text('shell', 'getprop', 'sys.boot_completed') != '1':
            raise RuntimeError('Wait for Android to finish booting.')
        if not re.search(r'\buid=0\(', text('exec-out', 'su', '-c', 'id')):
            raise RuntimeError('Root permission for the ADB shell is required.')
        memory = re.search(r'^MemTotal:\s+(\d+)\s+kB$', text('shell', 'cat', '/proc/meminfo'), re.M)
        if not memory or int(memory[1]) < 15000000:
            raise RuntimeError('This package has only been validated on the 16 GB RAM Ace 5 variant.')
        data = self.execute([self.adb, '-s', serial, 'exec-out', 'su', '-c', 'cat /sys/firmware/fdt'], capture=True)
        nodes = fdt_properties(data)
        compatibility = json.loads((self.root / 'compatibility.json').read_text(encoding='utf-8'))
        required = compatibility.get('dtb_properties')
        if not isinstance(required, dict) or not required:
            raise RuntimeError('No device-tree compatibility profile is present.')
        for path, properties in required.items():
            if not isinstance(properties, dict) or not properties:
                raise RuntimeError('Malformed compatibility profile')
            for name, expected in properties.items():
                if nodes.get(path, {}).get(name) != bytes.fromhex(expected):
                    raise RuntimeError(f'Hardware configuration differs from the tested Ace 5: {path}/{name}')
        return {'serial': serial, 'model': model, 'build': build,
                'dtb_sha256': hashlib.sha256(data).hexdigest()}, data

    def python(self, script, *args, timeout=60):
        return self.execute([sys.executable, '-X', 'utf8', self.source / script, *args], timeout=timeout)

    def phone(self, serial, *args, timeout=180):
        return self.python('tools/ace5/phone.py', '--serial', serial, *args, timeout=timeout)

    def check_backup(self, directory):
        data = json.loads((directory / 'manifest.json').read_text(encoding='utf-8'))
        if data.get('model') != MODEL or set(data.get('partitions', {})) != PARTITIONS:
            raise RuntimeError('The complete 22-partition verified boot-chain backup is required.')
        for name, entry in data['partitions'].items():
            checked_file(directory / (name + '.img'), entry)

    def prepare(self):
        self.verify()
        if self.state_path.exists():
            raise RuntimeError('This extracted copy is already prepared. Keep private/ as a backup; use a fresh extraction for another phone.')
        state, dtb = self.android()
        if shutil.disk_usage(self.root).free < 4 * 1024**3:
            raise RuntimeError('At least 4 GiB free space is required for this phone\'s backups and temporary files.')
        directory = self.root / 'private' / ('device-' + time.strftime('%Y%m%d-%H%M%S'))
        directory.mkdir(parents=True, exist_ok=False)
        (directory / 'device.dtb').write_bytes(dtb)
        state['directory'] = directory.relative_to(self.root).as_posix()
        state['prepared_at'] = time.strftime('%Y-%m-%dT%H:%M:%S%z')
        (directory / 'inventory.json').write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
        print('Reading and verifying this phone\'s boot-chain backup; personal data is not included.', flush=True)
        self.python('tools/ace5/backup.py', '--serial', state['serial'], '--output', directory / 'boot-chain', timeout=7200)
        self.check_backup(directory / 'boot-chain')
        # The loader records diagnostics here even though Android partitions
        # are not flashed. Preserve the pre-test prefix on the owner's PC.
        logdump = self.execute([self.adb, '-s', state['serial'], 'exec-out', 'su', '-c',
                                'head -c 2097152 /dev/block/by-name/logdump'], capture=True)
        if len(logdump) != 2097152:
            raise RuntimeError('Could not preserve the complete logdump prefix.')
        (directory / 'logdump-before-2MiB.bin').write_bytes(logdump)
        state['logdump_before_sha256'] = hashlib.sha256(logdump).hexdigest()
        image = self.source / 'out/giulia-kernel/hackios-giulia-kernel.img'
        self.python('tools/ace5/assemble-boot.py', '--components', self.root / 'components/uefi',
                    '--dtb', directory / 'device.dtb', '--stage', 'kernel', '--output', image)
        state['boot_image_sha256'] = digest(image)
        self.state_path.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
        print('Prepared. Android was not rebooted. Keep private/; never share this prepared copy.', flush=True)

    def prepared(self):
        state = json.loads(self.state_path.read_text(encoding='utf-8'))
        if state.get('model') != MODEL or state.get('build') != BUILD:
            raise RuntimeError('Unexpected prepared device identity')
        directory = inside(self.root, state['directory'])
        if not directory.resolve().is_relative_to((self.root / 'private').resolve()):
            raise RuntimeError('Prepared state must refer to private/')
        if digest(directory / 'device.dtb') != state['dtb_sha256']:
            raise RuntimeError('Prepared DTB changed')
        self.check_backup(directory / 'boot-chain')
        if digest(self.source / 'out/giulia-kernel/hackios-giulia-kernel.img') != state['boot_image_sha256']:
            raise RuntimeError('Prepared temporary boot image changed')
        return state

    def start(self):
        self.verify()
        state = self.prepared()
        live, _ = self.android()
        # android() checks the current semantic hardware profile. Firmware may
        # refresh /chosen seeds between Android boots without changing hardware.
        if any(live[key] != state[key] for key in ('serial', 'model', 'build')):
            raise RuntimeError('Connected phone differs from preparation. Use a fresh extraction for another phone.')
        serial = state['serial']
        kernel = self.root / 'kernels/mach.ace5'
        self.phone(serial, 'enter', '--stage', 'kernel')
        self.phone(serial, 'selftest', '--kernel', kernel)
        self.phone(serial, 'ramdisk', '--image', self.root / 'images/system.apfs', timeout=900)
        self.phone(serial, 'run', '--seconds', '0', '--bootcpu', '1', '--touch',
                   '--gpu-probe', '20', '--gpu-direct', '--gpu-keep', '--metal',
                   '--gpu-display', '--fb-filter', 'bilinear', '--kernel', kernel,
                   '--root-image', self.root / 'images/root-display.apfs', timeout=240)
        print('Startup observation finished. iOS has no automatic return timer. Use the return command when finished.', flush=True)

    def launch(self):
        if not self.state_path.exists():
            self.prepare()
        self.start()

    def reboot(self):
        # phone.py itself checks the guarded giulia loader before any reboot.
        # No Android reboot is attempted if the phone is already in Android.
        state = json.loads(self.state_path.read_text(encoding='utf-8'))
        if state.get('model') != MODEL or not re.fullmatch('[A-Za-z0-9_.:-]+', state.get('serial', '')):
            raise RuntimeError('Missing or invalid prepared identity')
        self.phone(state['serial'], 'reboot', timeout=90)
        deadline = time.monotonic() + 120
        while time.monotonic() < deadline:
            try:
                value = self.execute([self.adb, '-s', state['serial'], 'shell', 'getprop', 'sys.boot_completed'],
                                     timeout=5, capture=True)
                if value.strip() == b'1':
                    print('Android boot completed.', flush=True)
                    return
            except (RuntimeError, subprocess.TimeoutExpired):
                pass
            time.sleep(2)
        raise RuntimeError('Android boot completion is not yet confirmed. Inspect the phone; do not flash anything.')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('action', choices=('verify', 'prepare', 'start', 'launch', 'reboot'))
    args = parser.parse_args()
    try:
        getattr(Package(args.package), args.action)()
    except (RuntimeError, OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        print('Stopped: ' + str(error), file=sys.stderr)
        print('No flash/erase fallback is attempted. Preserve private/ and source/HackiOS-Ace5/logs/.', file=sys.stderr)
        if args.action in ('start', 'launch'):
            print('If the phone is stuck, use the documented power + volume-up forced restart to return to Android.', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
