#!/usr/bin/env python3
"""Build a PRIVATE small redwood RAM volume, never an iOS system image.

Use a trusted, quiescent, prepared userland/root directory (including an
already prepared Sileo redwood tree). The source tree is never modified.
Input and output paths for proprietary data must be outside this checkout.
No discovery, downloads, flashing, system-image editing, or hardware access.
"""
import argparse
import os
from pathlib import Path
import stat
import sys
import tempfile

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "tools" / "apfs"))
import mkapfs  # existing host writer; stdlib only

NAMES = ("amss20.bin", "amss.bin")
MAX_BYTES = 64 * 512 * 1024  # matches giulia_mhi_fw_plan
DIR = "kiwifw"


def linked(st):
    return stat.S_ISLNK(st.st_mode) or bool(
        getattr(st, "st_file_attributes", 0) & getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0x400))


def path_arg(value):
    p = Path(value)
    if not p.name or ".." in p.parts:
        raise ValueError("empty or traversing path")
    return Path(os.path.abspath(p))


def real_dir(p):
    # Do not resolve a directory symlink/junction and silently use its target.
    # On macOS /var may itself be a symlink; use its physical path explicitly.
    for d in (*reversed(p.parents), p):
        st = d.lstat()
        if linked(st) or not stat.S_ISDIR(st.st_mode):
            raise ValueError("a directory argument is not a physical directory")


def within(p, ancestor):
    return p == ancestor or ancestor in p.parents


def validate(device, root_tree, firmware_dir, output):
    if device != "giulia" or os.environ.get("SHARE") or os.environ.get("FLASHBOOT") or os.environ.get("COMPONENTS_ONLY") == "1":
        raise ValueError("private image requires giulia RAM mode; not SHARE, FLASHBOOT or COMPONENTS_ONLY")
    root, fw, out = map(path_arg, (root_tree, firmware_dir, output))
    for p in (root, fw, out.parent):
        real_dir(p)
    if within(fw, REPO) or within(out, REPO) or within(out, root) or within(out, fw) or within(fw, root) or within(root, fw):
        raise ValueError("firmware, root and output must be separate; private inputs/output must be outside checkout")
    if out.suffix.lower() != ".apfs" or os.path.lexists(out):
        raise ValueError("private output must be a fresh .apfs path; no overwrite")
    if any(name.casefold() == DIR for name in os.listdir(root)):
        raise ValueError("prepared root already contains a reserved kiwifw entry")
    return root, fw, out


def read_root(p, name=""):
    # mkapfs.read_tree uses these same node kinds; explicitly reject Windows
    # directory junctions and other reparse points instead of traversing them.
    st = p.lstat()
    if stat.S_ISLNK(st.st_mode):
        return mkapfs.Node(name, st.st_mode, target=os.fsencode(os.readlink(p)))
    if linked(st):
        raise ValueError("unexpected reparse point in prepared root")
    if stat.S_ISDIR(st.st_mode):
        n = mkapfs.Node(name, st.st_mode)
        for entry in sorted(os.listdir(p)):
            child = read_root(p / entry, entry)
            if child is not None:
                n.children[entry] = child
        return n
    if stat.S_ISREG(st.st_mode):
        return mkapfs.Node(name, st.st_mode, source=str(p))
    return None  # matches mkapfs (no devices, sockets, FIFOs)


def copy_candidate(fw, name, dest):
    p = fw / name
    try:
        before = p.lstat()
    except FileNotFoundError:
        return False
    if linked(before) or not stat.S_ISREG(before.st_mode) or not (0 < before.st_size <= MAX_BYTES):
        raise ValueError("allowlisted firmware must be a nonempty bounded regular file, not a link")
    flags = os.O_RDONLY | getattr(os, "O_BINARY", 0) | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0)
    with os.fdopen(os.open(p, flags), "rb") as f:
        opened = os.fstat(f.fileno())
        if not stat.S_ISREG(opened.st_mode) or not os.path.samestat(before, opened) or opened.st_size != before.st_size:
            raise ValueError("firmware candidate changed before open")
        with dest.open("xb") as wr:
            remaining = before.st_size
            while remaining:
                b = f.read(min(remaining, 256 * 1024))
                if not b:
                    raise ValueError("firmware candidate shortened during copy")
                wr.write(b)
                remaining -= len(b)
            if f.read(1) or os.fstat(f.fileno()).st_mtime_ns != opened.st_mtime_ns:
                raise ValueError("firmware candidate changed during copy")
    return True


def pack_private(device, root_tree, firmware_dir, output):
    root, fw, out = validate(device, root_tree, firmware_dir, output)
    with tempfile.TemporaryDirectory(prefix=".kiwi-private-", dir=out.parent) as tmp:
        tmp = Path(tmp)
        node = mkapfs.Node(DIR, stat.S_IFDIR | 0o755)
        # List names only to enforce exact case on case-insensitive volumes.
        present = set(os.listdir(fw))
        for name in NAMES:
            target = tmp / name
            if name in present:
                if not copy_candidate(fw, name, target):
                    raise ValueError("allowlisted candidate disappeared")
                node.children[name] = mkapfs.Node(name, stat.S_IFREG | 0o444, source=str(target))
        if not node.children:
            raise ValueError("no exact amss20.bin or amss.bin in supplied private directory")
        tree = read_root(root)
        if any(n.casefold() == DIR for n in tree.children):
            raise ValueError("prepared root acquired conflicting kiwifw entry")
        tree.children[DIR] = node  # virtual overlay, never through a filesystem link
        img = tmp / "private.apfs"
        os.close(os.open(img, os.O_CREAT | os.O_EXCL | os.O_WRONLY, 0o600))
        with tempfile.TemporaryFile(dir=tmp) as spool:
            vol = mkapfs.Volume(0, "redwood", tree, True, 1683900000 * 10**9, spool, ("fw", DIR))
            image = mkapfs.Container([vol], 256, "private-kiwi.apfs")
            image.layout()
            image.write(str(img))
        if img.stat().st_size > 32 * 1024 * 1024:
            raise ValueError("complete redwood RAM volume exceeds phone.py 32 MiB limit")
        # Link is atomic NOREPLACE on a single filesystem. On a filesystem
        # without hardlinks refuse rather than risk replacing an existing file.
        os.link(img, out)
    return tuple(node.children)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for a in ("device", "root-tree", "firmware-dir", "output"):
        ap.add_argument("--" + a, required=True)
    a = ap.parse_args()
    try:
        names = pack_private(a.device, a.root_tree, a.firmware_dir, a.output)
    except (OSError, ValueError) as err:
        # Never log private paths embedded in OS exceptions.
        reason = str(err) if isinstance(err, ValueError) else type(err).__name__
        ap.exit(1, "private KIWI RAM image not produced: " + reason + "\n")
    print("private KIWI RAM image created (redwood; uncompressed /kiwifw): " + ", ".join(names))


if __name__ == "__main__":
    main()
