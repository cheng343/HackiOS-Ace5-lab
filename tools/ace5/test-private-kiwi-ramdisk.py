#!/usr/bin/env python3
"""Offline private redwood RAM volume tests. Uses synthetic bytes; no compiler/device."""
import importlib.util
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location("packer", HERE / "private-kiwi-ramdisk.py")
packer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(packer)
import apfs


class RamdiskTests(unittest.TestCase):
    def setUp(self):
        self.env = patch.dict(os.environ, {k: "" for k in ("SHARE", "FLASHBOOT", "COMPONENTS_ONLY")})
        self.env.start()
        self.addCleanup(self.env.stop)
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.p = Path(tmp.name).resolve()
        self.root = self.p / "redwood"
        self.fw = self.p / "private"
        self.out = self.p / "private-out.apfs"
        self.root.mkdir()
        self.fw.mkdir()
        (self.root / "hello").write_bytes(b"ordinary redwood\n" * 900)
        self.a = b"opaque20!\x00\xff" * 777
        self.b = b"opaque-legacy!\xff" * 123

    def pack(self):
        return packer.pack_private("giulia", self.root, self.fw, self.out)

    def rejected(self):
        with self.assertRaises((ValueError, OSError)):
            self.pack()
        self.assertFalse(self.out.exists())
        self.assertFalse(list(self.p.glob(".kiwi-private-*")))

    def test_dual_exact_uncompressed_normal_files_and_unmodified_inputs(self):
        for name, data in zip(packer.NAMES, (self.a, self.b)):
            (self.fw / name).write_bytes(data)
        (self.fw / "ignore.bin").write_bytes(b"not for sharing")
        self.assertEqual(self.pack(), packer.NAMES)
        self.assertFalse((self.root / "kiwifw").exists())
        with self.assertRaises(ValueError):  # no-clobber
            self.pack()
        c = apfs.Container(str(self.out))
        try:
            v = apfs.pick_volume(c, "redwood")
            for name, expected in zip(packer.NAMES, (self.a, self.b)):
                ino = v.inode(v.lookup("/kiwifw/" + name)[0])
                self.assertIsNone(ino.decmpfs)
                self.assertEqual(ino.contents(), expected)
                self.assertEqual(ino.mode & 0o777, 0o444)
            self.assertIsNone(v.lookup("/kiwifw/ignore.bin"))
            ordinary = v.inode(v.lookup("/hello")[0])
            self.assertEqual(ordinary.contents(), (self.root / "hello").read_bytes())
            self.assertIsNotNone(ordinary.decmpfs)
        finally:
            c.close()

    def test_single_and_default_public_image_is_clean(self):
        (self.fw / "amss.bin").write_bytes(self.b)
        self.assertEqual(self.pack(), ("amss.bin",))
        public = self.p / "public.apfs"
        subprocess.run([sys.executable, str(packer.REPO / "tools/apfs/mkapfs.py"), "-z",
                        "--plain", "fw", "-o", str(public), "redwood=" + str(self.root)],
                       check=True, stdout=subprocess.PIPE, timeout=60)
        c = apfs.Container(str(public))
        try:
            self.assertIsNone(apfs.pick_volume(c, "redwood").lookup("/kiwifw"))
        finally:
            c.close()

    def test_missing_case_size_stale_and_unsafe_modes(self):
        self.rejected()
        (self.fw / "AMSS20.BIN").write_bytes(self.a)
        self.rejected()
        (self.fw / "AMSS20.BIN").unlink()
        (self.fw / "amss20.bin").touch()
        self.rejected()
        (self.fw / "amss20.bin").write_bytes(self.a)
        for env_key in ("SHARE", "FLASHBOOT", "COMPONENTS_ONLY"):
            with self.subTest(key=env_key), patch.dict(os.environ, {env_key: "1"}):
                self.rejected()
        with self.assertRaises(ValueError):
            packer.pack_private("redwood", self.root, self.fw, self.out)
        self.assertFalse(self.out.exists())
        (self.root / "KiWiFw").mkdir()
        self.rejected()

    def test_symlinks_not_followed_and_preserved(self):
        victim = self.p / "outside"
        victim.write_bytes(self.a)
        src = self.fw / "amss20.bin"
        try:
            src.symlink_to(victim)
        except OSError as e:
            self.skipTest("creating symbolic links unavailable on host: " + type(e).__name__)
        self.rejected()
        src.unlink()
        src.write_bytes(self.a)
        alias = self.root / "kiwifw"
        alias.symlink_to(self.fw, target_is_directory=True)
        self.rejected()
        alias.unlink()
        (self.root / "systemlink").symlink_to("/does/not/exist")
        self.pack()
        c = apfs.Container(str(self.out))
        try:
            v = apfs.pick_volume(c, "redwood")
            ino = v.inode(v.lookup("/systemlink")[0])
            self.assertTrue(ino.is_link)
            self.assertEqual(ino.symlink_target_bytes(), b"/does/not/exist")
        finally:
            c.close()
        self.assertEqual(victim.read_bytes(), self.a)

    def test_oversize_whole_volume_not_published(self):
        (self.fw / "amss20.bin").write_bytes(self.a)
        (self.root / "fw").mkdir()
        with (self.root / "fw/padded").open("wb") as f:
            f.truncate(32 * 1024 * 1024)  # kept plain; plus APFS metadata/other entries
        self.rejected()
        self.out.write_bytes(b"previous private image")
        with self.assertRaises(ValueError):
            self.pack()
        self.assertEqual(self.out.read_bytes(), b"previous private image")

    def test_isolation_traversal_reserved_output_and_no_private_path_in_errors(self):
        for out in (self.root / "output.apfs", self.fw / "out.apfs",
                    packer.REPO / "forbidden.apfs", self.p / "other" / ".." / "a.apfs"):
            with self.subTest(out=out.name), self.assertRaises(ValueError):
                packer.pack_private("giulia", self.root, self.fw, out)
        cmd = [sys.executable, str(HERE / "private-kiwi-ramdisk.py"), "--device", "giulia",
               "--root-tree", str(self.root), "--firmware-dir", str(self.fw / "missing-input"),
               "--output", str(self.out)]
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=60)
        self.assertNotEqual(r.returncode, 0)
        self.assertNotIn(str(self.p), r.stderr)
        self.assertNotIn("Traceback", r.stderr)
        self.assertFalse(self.out.exists())


if __name__ == "__main__":
    unittest.main()
