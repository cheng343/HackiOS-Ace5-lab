#!/usr/bin/env python3
"""Portable package checks with synthetic files and mocked device operations."""
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import tempfile
import unittest
from unittest.mock import patch


SPEC = importlib.util.spec_from_file_location('ace5_share', Path(__file__).with_name('share.py'))
share = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(share)


def make_fdt(compatible=b'example,phone\0'):
    strings = b'compatible\0'
    block = struct.pack('>I', 1) + b'\0' * 4
    block += struct.pack('>III', 3, len(compatible), 0) + compatible
    block += b'\0' * (-len(compatible) % 4)
    block += struct.pack('>II', 2, 9)
    offset = 56
    return (struct.pack('>10I', 0xd00dfeed, offset + len(block) + len(strings),
                        offset, offset + len(block), 40, 17, 16, 0, len(strings), len(block))
            + b'\0' * 16 + block + strings)


class TemporaryCase(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='ace5-share-test-',
                                               dir=Path(__file__).resolve().parents[3])
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()


class FileTests(TemporaryCase):
    def test_paths_reject_windows_and_posix_escape(self):
        for name in ('../outside', '/outside', 'a/../../outside', 'a\\b',
                     'C:/outside', 'a//b', './a', 'a/../b', ''):
            with self.subTest(name=name), self.assertRaises(RuntimeError):
                share.inside(self.root, name)
        self.assertEqual(share.inside(self.root, 'images/system.apfs'), self.root / 'images/system.apfs')

    def test_corrupt_or_short_asset_fails(self):
        asset = self.root / 'asset'
        asset.write_bytes(b'valid payload')
        entry = {'bytes': asset.stat().st_size, 'sha256': share.digest(asset)}
        share.checked_file(asset, entry)
        asset.write_bytes(b'wrong payload')
        with self.assertRaises(RuntimeError):
            share.checked_file(asset, entry)
        asset.write_bytes(b'valid payload extra')
        with self.assertRaises(RuntimeError):
            share.checked_file(asset, entry)

    def test_manifest_missing_required_payload_fails(self):
        (self.root / 'manifest.json').write_text('{"files": {}}', encoding='utf-8')
        with self.assertRaisesRegex(RuntimeError, 'Incomplete'):
            share.Package(self.root).verify()

    def test_partial_backup_is_rejected(self):
        (self.root / 'manifest.json').write_text(json.dumps({'model': share.MODEL,
                                                          'partitions': {'boot_a': {}}}))
        with self.assertRaisesRegex(RuntimeError, '22-partition'):
            share.Package(self.root).check_backup(self.root)

    def test_all_backup_partitions_require_matching_bytes(self):
        entries = {}
        for name in share.PARTITIONS:
            path = self.root / (name + '.img')
            path.write_bytes(name.encode())
            entries[name] = {'bytes': path.stat().st_size, 'sha256': share.digest(path)}
        (self.root / 'manifest.json').write_text(json.dumps({'model': share.MODEL, 'partitions': entries}))
        share.Package(self.root).check_backup(self.root)
        (self.root / 'boot_b.img').write_bytes(b'bad')
        with self.assertRaises(RuntimeError):
            share.Package(self.root).check_backup(self.root)


class FdtTests(unittest.TestCase):
    def test_semantic_property_read(self):
        self.assertEqual(share.fdt_properties(make_fdt()), {'/': {'compatible': b'example,phone\0'}})

    def test_truncated_and_wrong_header_fail(self):
        original = make_fdt()
        for value in (b'', original[:-1], b'XXXX' + original[4:]):
            with self.subTest(size=len(value)), self.assertRaises(RuntimeError):
                share.fdt_properties(value)

    def test_property_length_cannot_cross_structure(self):
        value = bytearray(make_fdt())
        struct.pack_into('>I', value, 68, 0xffffffff)
        with self.assertRaises(RuntimeError):
            share.fdt_properties(value)


class DevicePackage(share.Package):
    def __init__(self, root):
        super().__init__(root)
        self.calls = []
        self.devices = b'List of devices attached\nTEST\tdevice\n'
        self.model = share.MODEL
        self.build = share.BUILD
        self.memory = b'MemTotal:       15561224 kB\n'
        self.uid = b'uid=0(root) gid=0(root)\n'
        self.dtb = make_fdt()

    def execute(self, command, timeout=60, capture=False):
        self.calls.append([str(x) for x in command])
        args = tuple(str(x) for x in command[1:])
        if args == ('devices',):
            return self.devices
        tail = args[2:]
        responses = {
            ('shell', 'getprop', 'ro.product.model'): self.model.encode(),
            ('shell', 'getprop', 'ro.build.display.id'): self.build.encode(),
            ('shell', 'getprop', 'sys.boot_completed'): b'1\n',
            ('exec-out', 'su', '-c', 'id'): self.uid,
            ('shell', 'cat', '/proc/meminfo'): self.memory,
            ('exec-out', 'su', '-c', 'cat /sys/firmware/fdt'): self.dtb,
        }
        if tail not in responses:
            raise AssertionError('Unexpected device operation: ' + repr(command))
        return responses[tail]


class DeviceTests(TemporaryCase):
    def setUp(self):
        super().setUp()
        (self.root / 'compatibility.json').write_text(json.dumps({
            'dtb_properties': {'/': {'compatible': b'example,phone\0'.hex()}}}))
        self.package = DevicePackage(self.root)

    def test_expected_phone_passes_read_only_identity(self):
        state, dtb = self.package.android()
        self.assertEqual(state['serial'], 'TEST')
        self.assertEqual(state['dtb_sha256'], hashlib.sha256(dtb).hexdigest())
        self.assertTrue(all('reboot' not in call for call in self.package.calls))

    def test_wrong_model_build_memory_root_rejected(self):
        for attr, value in (('model', 'CPH2645'), ('build', 'different'),
                            ('memory', b'MemTotal: 11000000 kB\n'), ('uid', b'uid=2000(shell)')):
            with self.subTest(attr=attr):
                package = DevicePackage(self.root)
                setattr(package, attr, value)
                with self.assertRaises(RuntimeError):
                    package.android()
                self.assertTrue(all('reboot' not in call for call in package.calls))

    def test_multiple_and_unauthorized_devices_rejected(self):
        for value in (b'List of devices attached\n',
                      b'List of devices attached\nTEST\tunauthorized\n',
                      b'List of devices attached\nTEST\tdevice\nOTHER\tdevice\n'):
            with self.subTest(value=value):
                self.package.devices = value
                with self.assertRaises(RuntimeError):
                    self.package.android()

    def test_different_hardware_profile_rejected(self):
        self.package.dtb = make_fdt(b'example,other\0')
        with self.assertRaisesRegex(RuntimeError, 'Hardware configuration differs'):
            self.package.android()


class FlowPackage(share.Package):
    def __init__(self, root):
        super().__init__(root)
        self.events = []
        self.identity = {'serial': 'TEST', 'model': share.MODEL, 'build': share.BUILD, 'dtb_sha256': 'old'}
        self.live = dict(self.identity, dtb_sha256='fresh-seed')
        self.fail_at = None

    def verify(self):
        self.events.append('verify')

    def prepared(self):
        self.events.append('prepared')
        return self.identity

    def android(self):
        self.events.append('android')
        return self.live, b''

    def phone(self, serial, *args, timeout=180):
        self.events.append((serial, tuple(str(arg) for arg in args), timeout))
        if args[0] == self.fail_at:
            raise RuntimeError('mock failure')

    def prepare(self):
        self.events.append('prepare')


class FlowTests(TemporaryCase):
    def test_start_is_unlimited_without_automatic_reboot(self):
        package = FlowPackage(self.root)
        package.start()
        phone_calls = [event for event in package.events if isinstance(event, tuple)]
        self.assertEqual([event[1][0] for event in phone_calls], ['enter', 'selftest', 'ramdisk', 'run'])
        options = phone_calls[-1][1]
        self.assertEqual(options[options.index('--seconds') + 1], '0')
        self.assertEqual(options[options.index('--bootcpu') + 1], '1')
        self.assertIn('--gpu-keep', options)
        self.assertNotIn('--live-log', options)
        self.assertNotIn('reboot', [event[1][0] for event in phone_calls])

    def test_changed_phone_stops_before_any_boot(self):
        package = FlowPackage(self.root)
        package.live['serial'] = 'OTHER'
        with self.assertRaises(RuntimeError):
            package.start()
        self.assertFalse(any(isinstance(event, tuple) for event in package.events))

    def test_failed_selftest_never_uploads_or_runs(self):
        package = FlowPackage(self.root)
        package.fail_at = 'selftest'
        with self.assertRaises(RuntimeError):
            package.start()
        self.assertEqual([event[1][0] for event in package.events if isinstance(event, tuple)], ['enter', 'selftest'])

    def test_one_click_prepares_only_first_time(self):
        package = FlowPackage(self.root)
        package.launch()
        self.assertEqual(package.events[0], 'prepare')
        package.events.clear()
        package.state_path.parent.mkdir(parents=True)
        package.state_path.write_text('{}')
        package.launch()
        self.assertNotIn('prepare', package.events)

    def test_preparation_refuses_existing_state_before_device_access(self):
        package = share.Package(self.root)
        package.state_path.parent.mkdir(parents=True)
        package.state_path.write_text('{}')
        with patch.object(package, 'verify'), patch.object(package, 'android') as android:
            with self.assertRaisesRegex(RuntimeError, 'already prepared'):
                package.prepare()
            android.assert_not_called()


if __name__ == '__main__':
    unittest.main()
