#!/usr/bin/env python3
"""Direct temporary RAM launch using this phone's live DTB.

No model/build/RAM/profile or package-hash preflight, backup, or timed selftest.
The existing loader's allocation, protocol and GPU startup checks still apply.
"""
import argparse
import gzip
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import tomllib


class Direct:
    def __init__(self, root):
        self.root = root.resolve()
        self.source = self.root / 'source/HackiOS-Ace5'
        self.platform = self.root / 'tools/platform-tools'
        self.env = dict(os.environ)
        self.env['PATH'] = str(self.platform) + os.pathsep + self.env.get('PATH', '')
        self.env['PYTHONUTF8'] = '1'
        self.state_path = self.root / 'private/direct-last.json'

    def call(self, command, timeout=60, capture=False):
        result = subprocess.run([str(x) for x in command], env=self.env, timeout=timeout,
                                stdout=subprocess.PIPE if capture else None,
                                stderr=subprocess.PIPE if capture else None)
        if result.returncode:
            message = (result.stderr or b'').decode('utf-8', errors='replace').strip()
            raise RuntimeError(f'{Path(str(command[0])).name} exited {result.returncode}: {message}')
        return result.stdout if capture else b''

    def adb(self, *args, **kwargs):
        return self.call([self.platform / 'adb.exe', *args], **kwargs)

    def fb(self, serial, *args, **kwargs):
        return self.call([self.platform / 'fastboot.exe', '-s', serial, *args], **kwargs)

    def phone(self, serial, *args, timeout=180):
        return self.call([sys.executable, '-X', 'utf8', self.source / 'tools/ace5/phone.py',
                          '--serial', serial, *args], timeout=timeout)

    def wait_fastboot(self, serial):
        for _ in range(60):
            value = self.call([self.platform / 'fastboot.exe', 'devices'], capture=True).decode('utf-8', errors='replace')
            if any(line.split() and line.split()[0] == serial for line in value.splitlines()):
                return
            time.sleep(1)
        raise RuntimeError('Fastboot device did not appear. Check the USB driver and phone screen.')

    def prepare(self):
        value = self.adb('devices', capture=True).decode('utf-8', errors='replace')
        devices = [line.split() for line in value.splitlines()[1:] if line.strip()]
        # A transport must be selected unambiguously; this is not a model/profile check.
        if len(devices) != 1 or len(devices[0]) < 2 or devices[0][1] != 'device':
            raise RuntimeError('Connect one Android phone and authorize USB debugging.')
        serial = devices[0][0]
        print('直接模式：读取本机 DTB；跳过机型、版本、内存和整包哈希预检。', flush=True)
        dtb = self.adb('-s', serial, 'exec-out', 'su', '-c', 'cat /sys/firmware/fdt', capture=True)
        if not dtb:
            raise RuntimeError('No live DTB received. Allow root access for the ADB shell.')
        directory = self.root / 'private' / ('direct-' + str(time.time_ns()))
        directory.mkdir(parents=True, exist_ok=False)
        dtb_path = directory / 'device.dtb'
        dtb_path.write_bytes(dtb)
        components = self.root / 'components/uefi'
        blob = ((components / 'BootShim.bin').read_bytes() +
                (components / 'SILICIUM_UEFI.fd').read_bytes() +
                (components / 'giulia-kernel/usb-payload.bin').read_bytes())
        blob += b'\0' * (-len(blob) % 16)
        packed = directory / 'kernel.gz-dtb'
        packed.write_bytes(gzip.compress(blob, mtime=0) + dtb)
        image = directory / 'temporary-boot.img'
        boot = tomllib.loads((components / 'giulia.toml').read_text(encoding='utf-8'))['boot_image']
        self.call([sys.executable, '-X', 'utf8', components / 'mkbootimg.py',
                   '--kernel', packed, '--ramdisk', components / 'ramdisk',
                   '--header_version', str(boot['header_version']), '--os_version', boot['os_version'],
                   '--os_patch_level', boot['os_patch_level'], '-o', image])
        state = {'serial': serial, 'directory': directory.relative_to(self.root).as_posix(),
                 'temporary_image': image.relative_to(self.root).as_posix(), 'backup_created': False}
        self.state_path.write_text(json.dumps(state, indent=2) + '\n', encoding='utf-8')
        return state

    def launch(self):
        state = self.prepare()
        serial = state['serial']
        print('准备完成，正在临时启动。此直接模式不会创建启动分区备份。', flush=True)
        self.adb('-s', serial, 'reboot', 'bootloader')
        self.wait_fastboot(serial)
        self.fb(serial, 'boot', self.root / state['temporary_image'], timeout=60)
        self.wait_fastboot('xnuloader')
        self.phone(serial, 'ramdisk', '--image', self.root / 'images/system.apfs', timeout=900)
        self.phone(serial, 'run', '--seconds', '0', '--bootcpu', '1', '--touch',
                   '--gpu-probe', '20', '--gpu-direct', '--gpu-keep', '--metal',
                   '--gpu-display', '--fb-filter', 'bilinear',
                   '--kernel', self.root / 'kernels/mach.ace5',
                   '--root-image', self.root / 'images/root-display.apfs', timeout=240)
        print('iOS 持续运行，无自动返回计时。需要退出时运行“返回安卓.cmd”。', flush=True)

    def reboot(self):
        state_path = self.state_path if self.state_path.exists() else self.root / 'private/prepared.json'
        state = json.loads(state_path.read_text(encoding='utf-8'))
        self.phone(state['serial'], 'reboot', timeout=90)
        print('已请求返回 Android，请等待手机正常开机。', flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--package', type=Path, default=Path(__file__).resolve().parents[4])
    parser.add_argument('action', choices=('launch', 'prepare', 'reboot'))
    args = parser.parse_args()
    try:
        getattr(Direct(args.package), args.action)()
    except (OSError, ValueError, KeyError, RuntimeError, subprocess.TimeoutExpired) as error:
        print('停止：' + str(error), file=sys.stderr)
        print('保留窗口错误信息。若 iOS 卡住，可长按电源键 + 音量上键重启回 Android。', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
