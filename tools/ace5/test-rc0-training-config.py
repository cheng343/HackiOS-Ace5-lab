# SPDX-License-Identifier: BSD-2-Clause
"""Synthetic only: test the RC0 walker, never a PCI device or ADB."""
import json
import unittest
from rc0_training_config import inspect_root, EXT_LIMIT, STD_LIMIT


def ext(cid, ver=1, nxt=0):
    return cid | ver << 16 | nxt << 20


def fixture():
    # Type1 bridge; PM -> PCIe v2 root -> MSI. Deliberately non-monotonic
    # extended list 0x100 -> 0x220 -> 0x180, with SECPCI in the middle.
    return {0: 0x011317cb, 4: 0x00100004, 8: 0x06040001, 0x0c: 0x00810000,
            0x34: 0x40, 0x40: 0x7001, 0x70: 0x0042b010, 0xb0: 0x00800005,
            0x7c: 0x007b4c23, 0x100: ext(1, 2, 0x220), 0x220: ext(0x19, 1, 0x180),
            0x180: ext(0x1e), 0x22c: 0x2a174915}


class Reader:
    def __init__(self, words):
        self.words, self.calls, self.count = words, [], {}

    def __call__(self, off):
        self.calls.append(off)
        self.count[off] = n = 1 + self.count.get(off, 0)
        try:
            value = self.words[off]
        except KeyError:
            raise OSError(f'missing dword at {off:#x}') from None
        return value(n) if callable(value) else value


class RC0(unittest.TestCase):
    def test_realistic_non_monotone_and_minimal_reads(self):
        words = fixture()
        read = Reader(words)
        out = inspect_root(read)
        self.assertEqual(json.loads(json.dumps(out)), out)
        self.assertEqual((out['status'], out['offset'], out['raw'], out['lanes']),
                         ('present', 0x220, 0x2a174915, [0x4915, 0x2a17]))
        self.assertEqual((out['pcie_offset'], out['link_capabilities']), (0x70, words[0x7c]))
        self.assertEqual(out['extended_headers'], [{'offset': p, 'raw': words[p]} for p in (0x100, 0x220, 0x180)])
        self.assertEqual(set(read.calls), set(words))
        for p, count in read.count.items():
            self.assertEqual(count, 1 if p == 0x22c else 2)
        self.assertEqual(read.calls[-1], 0)

    def test_complete_absence(self):
        for head in (0, ext(1), ext(1, 2, 0x180)):
            out = inspect_root(Reader(fixture() | {0x100: head}))
            self.assertEqual((out['status'], out['offset'], out['raw'], out['lanes']), ('absent', None, None, []))

    def test_zero_lane_settings_are_readable(self):
        self.assertEqual(inspect_root(Reader(fixture() | {0x22c: 0}))['lanes'], [0, 0])

    def test_identity_and_accessibility(self):
        for off, val in [(0, 0x110717cb), (0, 0xffffffff), (8, 0x02800001), (0x0c, 0),
                         (4, 0), (0x34, 0), (0x34, 0x41), (0x40, 0x7000), (0x40, 0xffffffff),
                         (0x70, 0x0040b010), (0x70, 0x0043b010), (0x70, 0x0002b010),
                         (0xb0, 0x00420010), (0x7c, 0x007b4c13), (0x7c, 0x007b4c22),
                         (0x100, None), (0x100, True), (0x100, -1), (0x100, 1 << 32),
                         (0x100, 0xffffffff), (0x180, 0), (0x22c, 0xffff1234), (0x22c, 0xffff)]:
            with self.subTest(off=hex(off), value=val), self.assertRaises(ValueError):
                inspect_root(Reader(fixture() | {off: val}))
        for missing in (0, 4, 0x100, 0x180, 0x22c):
            with self.subTest(missing=hex(missing)), self.assertRaises(OSError):
                words = fixture(); del words[missing]; inspect_root(Reader(words))

    def test_caps_cannot_overlap_truncate_cycle_or_duplicate(self):
        cases = [{0x70: 0x0042fc10, 0xfc: 0x00420010},
                 # A standard cap points at the link register (must not walk it).
                 {0x70: 0x00427c10}, {0x40: 0x4001},
                 {0x100: ext(1, 1, 0x222)}, {0x100: ext(1, 1, 0x40)}, {0x100: ext(1, 1, 0x100)},
                 {0x100: ext(1, 1, 0xff4), 0xff4: ext(0x19)}, {0x180: ext(0x19)},
                 {0x220: ext(0x19, 1, 0x22c)}, {0x220: ext(0x19, 2)},
                 {0x180: ext(1, 1, 0x224)}, {0x180: ext(1, 1, 0x100)},
                 {0x100: ext(1, 1, 0x224), 0x224: ext(1, 1, 0x220)},
                 {0x100: ext(0xffff)}, {0x100: ext(0, 1, 0x220)}, {0x100: ext(1, 0, 0x220)}]
        for changes in cases:
            with self.subTest(changes=changes), self.assertRaises(ValueError):
                inspect_root(Reader(fixture() | changes))
        read = Reader(fixture() | {0x220: ext(0x19, 1, 0x22c)})
        with self.assertRaises(ValueError): inspect_root(read)
        self.assertNotIn(0x22c, read.calls)
        # Last legal target begins at 0xff0; last data dword 0xffc.
        out = inspect_root(Reader(fixture() | {0x100: ext(1, 1, 0xff0), 0xff0: ext(0x19), 0xffc: 0x12345678}))
        self.assertEqual(out['offset'], 0xff0)

    def test_layout_reread_and_bound(self):
        for p, changed in [(0, 0), (8, 0x05000001), (0x0c, 0), (4, 0), (0x34, 0x80),
                           (0x40, 0x8001), (0x70, 0x00420010), (0x7c, 0x7b4c13),
                           (0x100, ext(1)), (0x220, ext(1)), (0x180, ext(2))]:
            words = fixture()
            old = words[p]; words[p] = lambda n: old if n == 1 else changed
            with self.subTest(changed=hex(p)), self.assertRaises(ValueError):
                inspect_root(Reader(words))
        # The non-PCIe MSI control word can vary, without a layout change.
        self.assertEqual(inspect_root(Reader(fixture() | {0xb0: lambda n: (n << 16) | 5}))['status'], 'present')
        # Exhaustion after finding the target still is not a complete report.
        words = fixture() | {0x100: ext(0x19, 1, 0x200)}
        for i in range(EXT_LIMIT+1):
            words[0x200+4*i] = ext(1, 1, 0x204+4*i)
        read = Reader(words)
        with self.assertRaises(ValueError): inspect_root(read)
        self.assertLessEqual(len([p for p in read.calls if p>=0x100]), EXT_LIMIT)
        self.assertNotIn(0x10c, read.calls)
        words = fixture()
        for i in range(STD_LIMIT+1):
            words[0x40+4*i] = ((0x44+4*i) << 8) | 1
        with self.assertRaises(ValueError): inspect_root(Reader(words))


if __name__ == '__main__':
    unittest.main()
