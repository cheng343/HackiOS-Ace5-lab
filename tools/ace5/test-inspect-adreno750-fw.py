#!/usr/bin/env python3
"""Synthetic regression cases for the actual GMU firmware inspector.

No vendor firmware, device data, phone access or external dependencies are used.
"""
import importlib.util
from pathlib import Path
import struct
import unittest


SOURCE = Path(__file__).with_name('inspect-adreno750-fw.py')
SPEC = importlib.util.spec_from_file_location('ace5_firmware_inspector', SOURCE)
INSPECTOR = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSPECTOR)


def metadata(kind=1, value=4096, address=0x4000):
    return struct.pack('<4I', address, 0, kind, value)


def data_block(address=0x4000):
    return struct.pack('<4I', address, 4, 0, 0) + struct.pack('<I', 0x12345678)


class FirmwareInspectorTests(unittest.TestCase):
    def test_minimal_data_still_accepted(self):
        result = INSPECTOR.parse_gmu(data_block())
        self.assertEqual(len(result['blocks']), 1)
        self.assertEqual(result['blocks'][0]['region'], 'cache')

    def test_metadata_before_data_still_accepted(self):
        result = INSPECTOR.parse_gmu(b''.join(metadata(kind) for kind in range(1, 8)) + data_block())
        self.assertEqual(len(result['blocks']), 8)

    def test_all_metadata_types_rejected_after_data(self):
        for kind in range(1, 8):
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, 'metadata follows data'):
                INSPECTOR.parse_gmu(data_block() + metadata(kind))

    def test_32_blocks_accepted(self):
        result = INSPECTOR.parse_gmu(metadata() * 31 + data_block())
        self.assertEqual(len(result['blocks']), 32)

    def test_33_blocks_rejected(self):
        with self.assertRaisesRegex(ValueError, '32-block'):
            INSPECTOR.parse_gmu(metadata() * 32 + data_block())

    def test_reservation_boundary_accepted(self):
        for kind in (1, 7):
            with self.subTest(kind=kind):
                result = INSPECTOR.parse_gmu(metadata(kind, 1024 * 1024) + data_block())
                self.assertEqual(result['blocks'][0]['reservation_bytes'], 1024 * 1024)

    def test_reservation_above_boundary_rejected(self):
        for kind in (1, 7):
            for value in (1024 * 1024 + 4, 2 * 1024 * 1024):
                with self.subTest(kind=kind, value=value), self.assertRaisesRegex(ValueError, '1 MiB'):
                    INSPECTOR.parse_gmu(metadata(kind, value) + data_block())

    def test_existing_rejections_remain(self):
        malformed = (
            b'', data_block()[:-1], metadata(), metadata(99) + data_block(),
            metadata(value=0) + data_block(), metadata(address=0x4001) + data_block(),
            data_block() + data_block(),
        )
        for index, payload in enumerate(malformed):
            with self.subTest(index=index), self.assertRaises(ValueError):
                INSPECTOR.parse_gmu(payload)


if __name__ == '__main__':
    unittest.main(verbosity=2)
