#!/usr/bin/env python3
"""Exercise resource-fork recovery, including NTFS streams on Windows."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

exe = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory() as tmp:
    for size in [0, 19, 65536 + 37]:
        path = Path(tmp) / ('file-%d' % size)
        path.write_bytes(b'')
        data = bytes((i * 31 + (i >> 8)) & 255 for i in range(size))
        blocks = [data[i:i + 65536] for i in range(0, size, 65536)]
        offset = 4 * (len(blocks) + 1)
        offsets = [offset]
        for block in blocks:
            offset += len(block) + 1
            offsets.append(offset)
        fork = struct.pack('<' + 'I' * len(offsets), *offsets) + b''.join(b'\xff' + b for b in blocks)
        Path(str(path) + ':com.apple.decmpfs').write_bytes(struct.pack('<4sIQ', b'fpmc', 14, size))
        Path(str(path) + ':com.apple.ResourceFork').write_bytes(fork)
        subprocess.run([exe, str(path)], check=True)
        assert path.read_bytes() == data
        assert not Path(str(path) + ':com.apple.decmpfs').exists()
        assert not Path(str(path) + ':com.apple.ResourceFork').exists()
    bad = Path(tmp) / 'bad'
    bad.write_bytes(b'original')
    Path(str(bad) + ':com.apple.decmpfs').write_bytes(struct.pack('<4sIQ', b'fpmc', 14, 100))
    Path(str(bad) + ':com.apple.ResourceFork').write_bytes(b'\x00')
    assert subprocess.run([exe, str(bad)]).returncode != 0
    assert bad.read_bytes() == b'original'
print('unzbm: empty, multi-block and malformed resource forks passed')
