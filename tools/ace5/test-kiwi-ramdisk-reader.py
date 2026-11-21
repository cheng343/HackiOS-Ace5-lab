#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-2-Clause
"""Private writer -> real APFS container -> production kernel reader -> KIWI planner.
--check-only checks dependencies without a compiler; full suite runs on hosted Linux.
All fixtures are synthetic and live outside the checkout. No device or network I/O.
"""
import argparse
import ast
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
APFS = ROOT / "xnu/files/bsd/miscfs/apfs"
DEV = ROOT / "xnu/files/bsd/dev"
WRITER = ROOT / "tools/apfs/mkapfs.py"
PACKER = HERE / "private-kiwi-ramdisk.py"
C = HERE / "test-kiwi-ramdisk-reader.c"
SHIM = ROOT / "tools/apfs/host-shim"
SOURCES = [APFS / x for x in ("apfs_lib.c", "apfs_deflate.c", "apfs_zbitmap.c")]
SOURCES += [DEV / x for x in ("giulia_kiwi_reg.c", "giulia_mhi_firmware.c")]
ABSENT = "@absent"
LIMIT = 32 * 1024 * 1024


def inputs():
    for p in (Path(__file__), PACKER, WRITER):
        ast.parse(p.read_bytes(), filename=str(p))
    for p in [C, *SOURCES, APFS / "apfs_lib.h", DEV / "giulia_kiwi_image.h",
              SHIM / "x86/decompress_impl.h", DEV / "redwood_image.c"]:
        if not p.is_file():
            raise RuntimeError("missing input: " + str(p))
    if "redwood_image_boot_plain_file" not in (DEV / "redwood_image.c").read_text():
        raise RuntimeError("production strict reader not present")


def run(*argv):
    subprocess.run(list(map(str, argv)), check=True)


def shorten(good, payload, out):
    # A negative fixture: shorten the sole first extent, not the inode size,
    # then reseal its FS node so we specifically test the in-place admission.
    sys.path.insert(0, str(WRITER.parent))
    import mkapfs
    img = bytearray(good.read_bytes())
    data = payload.read_bytes()
    off = img.find(data)
    if off <= 0 or off % 4096 or img.find(data, off + 1) != -1:
        raise AssertionError("ambiguous payload location in generated image")
    rounded = (len(data) + 4095) // 4096 * 4096
    entry = struct.pack("<QQQ", rounded, off // 4096, 0)
    at = img.find(entry)
    if at < 0 or img.find(entry, at + 1) != -1 or at % 4096 > 4096 - len(entry):
        raise AssertionError("ambiguous extent location in generated image")
    start = at - at % 4096
    def checksum():
        return mkapfs.fletcher64(img[start:start + 4096])
    if struct.unpack_from("<Q", img, start)[0] != checksum():
        raise AssertionError("extent is not in a valid metadata block")
    struct.pack_into("<Q", img, at, 4096)
    struct.pack_into("<Q", img, start, checksum())
    out.write_bytes(img)


def declared_double_volume(src, out):
    # The controlled small mkapfs fixture builds both volumes and their OIDs, but
    # calculates nx_max_file_systems from container capacity (=1 here). Keep
    # that source byte-for-byte, and re-declare its two existing volumes in a
    # second, well-formed fixture. Both the initial NXSB and the transaction
    # checkpoint NXSB must agree; modifying only the initial copy is ineffective.
    sys.path.insert(0, str(WRITER.parent))
    import mkapfs
    bs = 4096
    if not bs <= src.stat().st_size <= LIMIT:
        raise AssertionError("two-volume source outside synthetic size bounds")
    img = bytearray(src.read_bytes())
    nx = img[:bs]
    if (len(img) % bs or nx[32:36] != b"NXSB" or struct.unpack_from("<I", nx, 36)[0] != bs
            or struct.unpack_from("<Q", nx, 40)[0] != len(img) // bs
            or struct.unpack_from("<Q", nx, 0)[0] != mkapfs.fletcher64(nx)):
        raise AssertionError("truncated, non-4KiB or unsealed synthetic container")
    declared = struct.unpack_from("<I", nx, 180)[0]
    a, b = struct.unpack_from("<QQ", nx, 184)
    if declared != 1 or not a or not b or a == b or any(nx[200:184 + 100 * 8]):
        raise AssertionError("expected precisely two populated OIDs and one declared slot")
    desc_base = struct.unpack_from("<Q", nx, 112)[0]
    desc_blocks = struct.unpack_from("<I", nx, 104)[0]
    if desc_base == 0 or desc_blocks < 2 or desc_base + desc_blocks > len(img) // bs:
        raise AssertionError("invalid synthetic checkpoint descriptor ring")
    # Writer: first ring block is CPMAP; the next is a byte-identical NXSB.
    copy = (desc_base + 1) * bs
    if img[copy:copy + bs] != nx:
        raise AssertionError("selected checkpoint does not match block-zero NXSB")
    struct.pack_into("<I", nx, 180, 2)
    struct.pack_into("<Q", nx, 0, mkapfs.fletcher64(nx))
    img[:bs] = nx
    img[copy:copy + bs] = nx
    with out.open("xb") as dst:
        dst.write(img)


def suite(tmp, cc):
    exe = tmp / "reader"
    run(cc, "-std=c11", "-D_DEFAULT_SOURCE", "-O1", "-I", APFS, "-I", DEV, "-I", SHIM,
        C, *SOURCES, "-lz", "-lcrypto", "-pthread", "-o", exe)
    root, fw = tmp / "root-tree", tmp / "firmware"
    root.mkdir()
    fw.mkdir()
    (root / "aaaa-other-entry").write_bytes(b"not firmware; inode numbers depend on the tree")
    name = "amss20.bin"
    payload = fw / name
    n = 512 * 1024 + 73
    # Highly compressible; missing --plain in the actual packer cannot pass as zero-copy.
    pattern = bytes((i * 31 + 53) % 251 for i in range(8192))
    payload.write_bytes((pattern * (n // len(pattern) + 1))[:n])
    good = tmp / "private.apfs"
    run(sys.executable, "-B", PACKER, "--device", "giulia", "--root-tree", root,
        "--firmware-dir", fw, "--output", good)
    if (root / "kiwifw").exists() or good.stat().st_size > LIMIT:
        raise AssertionError("source mutation or excessive final image size")

    def tree(label):
        d = tmp / label
        (d / "kiwifw").mkdir(parents=True)
        return d

    def mk(label, volumes, *flags):
        out = tmp / (label + ".apfs")
        run(sys.executable, "-B", WRITER, *flags, "-o", out, *(k + "=" + str(v) for k, v in volumes))
        return out

    def test(md0, md1, key, cap, result, disk=-1, mode="strict"):
        run(exe, mode, md0, md1, key, cap, result, disk)

    old = tree("other")
    (old / "kiwifw" / name).write_bytes(b"different older version")
    base = old / "kiwifw/amss.bin"
    base.write_bytes(b"only on the alternate root")
    fallback = mk("fallback", [("redwood", old)])
    test(fallback, good, name, n, payload, 1)
    test(good, ABSENT, name, n, payload, 0)
    test(fallback, ABSENT, "amss.bin", LIMIT, base, 0)
    test(fallback, good, name, n - 1, "invalid")
    test(fallback, good, "amss.bin", LIMIT, "absent")
    test(good, "@unreadable", name, LIMIT, "invalid")
    test(ABSENT, ABSENT, name, LIMIT, "absent")
    test(good, mk("wrong", [("System", old)]), name, LIMIT, "invalid")
    multi = mk("multi", [("redwood", old), ("other", root)])
    declared = tmp / "two-declared-volumes.apfs"
    declared_double_volume(multi, declared)
    test(good, multi, name, LIMIT, "invalid")      # extra OID past declared range
    test(good, declared, name, LIMIT, "invalid")   # ordinary, declared two-volume container
    broken = tmp / "broken.apfs"
    broken.write_bytes(bytes(8192))
    test(good, broken, name, LIMIT, "invalid")
    short = tmp / "short.apfs"
    shorten(good, payload, short)
    test(fallback, short, name, LIMIT, "invalid")
    zipped = tree("zipped")
    (zipped / "kiwifw" / name).write_bytes(payload.read_bytes())
    compressed = mk("compressed", [("redwood", zipped)], "-z")
    test(good, compressed, name, LIMIT, "invalid")
    test(good, compressed, name, LIMIT, payload, 1, "legacy-copy")
    edge = tree("edge")
    (edge / "kiwifw" / name).write_bytes(b"")
    (edge / "kiwifw/amss.bin").mkdir()
    bad = mk("types", [("redwood", edge)])
    test(fallback, bad, name, LIMIT, "invalid")
    test(fallback, bad, "amss.bin", LIMIT, "invalid")
    links = tree("links")
    (links / "kiwifw/amss.bin").write_bytes(b"regular target but different filename")
    (links / "kiwifw" / name).symlink_to("amss.bin")
    test(good, mk("symlink", [("redwood", links)]), name, LIMIT, "invalid")
    print("KIWI RAM disk: production pack, real APFS reader, CPU resolver and plan; "
          "in-place bytes, selection, invalid/absent and generic decompression passed")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cc", default="cc", help="cloud host C compiler")
    ap.add_argument("--check-only", action="store_true")
    args = ap.parse_args()
    inputs()
    if args.check_only:
        print("KIWI RAM disk reader inputs checked; C and image tests not run")
        return
    with tempfile.TemporaryDirectory(prefix="kiwi-synthetic-") as td:
        tmp = Path(td).resolve()
        if ROOT in tmp.parents or tmp == ROOT:
            raise RuntimeError("use temporary storage outside the checkout")
        suite(tmp, args.cc)


if __name__ == "__main__":
    main()
