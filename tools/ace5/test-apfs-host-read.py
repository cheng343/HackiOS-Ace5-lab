#!/usr/bin/env python3
"""Check binary APFS host reads at independent offsets, including on Windows."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'apfs'))
from apfs import RawImage

data = bytes(range(256)) * 1024
with tempfile.TemporaryDirectory() as folder:
    path = Path(folder) / 'raw'
    path.write_bytes(data)
    image = RawImage(str(path))
    try:
        requests = [(i * 997 % len(data), 1703) for i in range(400)]
        def check(request):
            offset, length = request
            assert image.read(offset, length) == data[offset:offset + length]
        with ThreadPoolExecutor(max_workers=8) as pool:
            list(pool.map(check, requests))
        assert image.read(len(data) + 1, 17) == b''
    finally:
        image.close()
print('APFS host reads: binary data, concurrent offsets and EOF passed')
