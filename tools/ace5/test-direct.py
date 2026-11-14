#!/usr/bin/env python3
"""Verify direct launch packing and command order without touching a phone."""
import gzip
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('ace5_direct', Path(__file__).with_name('direct.py'))
direct = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(direct)


class Fake(direct.Direct):
    def __init__(self, root):
        super().__init__(root)
        self.commands = []
        self.devices = b'List of devices attached\nEXAMPLE\tdevice\n'
        self.dtb = b'local device tree bytes'
        self.fail_at = None

    def call(self, command, timeout=60, capture=False):
        args = [str(x) for x in command]
        self.commands.append(args)
        if self.fail_at and self.fail_at in args:
            raise RuntimeError('test transport failure')
        if args[-1] == 'devices':
            return self.devices
        if args[-1] == 'cat /sys/firmware/fdt':
            return self.dtb
        if str(self.root / 'components/uefi/mkbootimg.py') in args:
            Path(args[args.index('-o') + 1]).write_bytes(b'temporary image')
        return b''

    def wait_fastboot(self, serial):
        self.commands.append(['wait-fastboot', serial])


class Tests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(dir=Path(__file__).resolve().parents[3])
        self.root = Path(self.temp.name)
        components = self.root / 'components/uefi'
        for name, content in [('BootShim.bin', b'shim'), ('SILICIUM_UEFI.fd', b'fd'),
                              ('giulia-kernel/usb-payload.bin', b'payload'),
                              ('giulia.toml', b'[boot_image]\nheader_version = 4\nos_version = "16.0.0"\nos_patch_level = "2026-07"\n')]:
            path = components / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(content)
        self.kit = Fake(self.root)

    def tearDown(self):
        self.temp.cleanup()

    def test_prepare_uses_own_tree_and_packs_without_compatibility_queries(self):
        state = self.kit.prepare()
        directory = self.root / state['directory']
        packed = (directory / 'kernel.gz-dtb').read_bytes()
        self.assertTrue(packed.endswith(self.kit.dtb))
        self.assertEqual(gzip.decompress(packed[:-len(self.kit.dtb)]),
                         b'shimfdpayload' + b'\0' * 3)
        self.assertEqual((directory / 'device.dtb').read_bytes(), self.kit.dtb)
        text = json.dumps(self.kit.commands)
        for forbidden in ('getprop', '/proc/meminfo', 'sha256sum', 'backup.py', 'reboot'):
            self.assertNotIn(forbidden, text)

    def test_launch_stays_running_without_selftest_or_auto_reboot(self):
        self.kit.launch()
        commands = self.kit.commands
        text = json.dumps(commands)
        self.assertNotIn('selftest', text)
        self.assertNotIn('flash', text)
        self.assertNotIn('erase', text)
        run = next(args for args in commands if 'run' in args)
        self.assertEqual(run[run.index('--seconds') + 1], '0')
        self.assertIn('--gpu-keep', run)
        self.assertIn('--touch', run)
        self.assertEqual(commands[-1], run)

    def test_transport_ambiguity_stops_before_reboot(self):
        for devices in (b'List of devices attached\n',
                        b'List of devices attached\nA\tdevice\nB\tdevice\n',
                        b'List of devices attached\nA\tunauthorized\n'):
            self.kit.devices = devices
            with self.assertRaises(RuntimeError):
                self.kit.launch()
            self.assertFalse(any('reboot' in args for args in self.kit.commands))

    def test_missing_dtb_stops_before_reboot(self):
        self.kit.dtb = b''
        with self.assertRaises(RuntimeError):
            self.kit.launch()
        self.assertFalse(any('reboot' in args for args in self.kit.commands))

    def test_upload_failure_does_not_start_kernel(self):
        self.kit.fail_at = 'ramdisk'
        with self.assertRaises(RuntimeError):
            self.kit.launch()
        self.assertFalse(any('run' in args for args in self.kit.commands))

    def test_explicit_return_uses_own_saved_session(self):
        state = self.kit.prepare()
        self.kit.commands.clear()
        self.kit.reboot()
        self.assertIn(state['serial'], self.kit.commands[-1])
        self.assertEqual(self.kit.commands[-1][-1], 'reboot')


if __name__ == '__main__':
    unittest.main()
