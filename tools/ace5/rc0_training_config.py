# SPDX-License-Identifier: BSD-2-Clause
"""Offline PCIe capability walker for one PKG110 root port. No transport or writes."""
STD_LIMIT = 32
EXT_LIMIT = 32


def inspect_root(read32):
    """read32(config_offset) -> little-endian, unsigned 32-bit integer.

    The transport must reject incomplete reads, distinguish errors from data,
    and select 0000:00:00.0 on the intended board. 'absent' means a stable,
    completely walked chain only. OSError/ValueError means unknown, not absent.
    This sequential observation is not a cold-training result or a PM lease.
    """
    observations = []

    def read(off):
        if not 0 <= off <= 0xffc or off & 3:
            raise ValueError('invalid configuration dword offset')
        v = read32(off)
        if type(v) is not int or not 0 <= v <= 0xffffffff or v == 0xffffffff:
            raise ValueError(f'invalid/inaccessible dword at {off:#x}')
        return v

    def field(off, mask=0xffffffff):
        value = read(off) & mask
        observations.append((off, mask, value))
        return value

    def check_ptr(p, low, high):
        if not low <= p <= high or p & 3:
            raise ValueError(f'malformed capability pointer {p:#x}')

    if field(0) != 0x011317cb or field(8, 0xffff0000) != 0x06040000:
        raise ValueError('not the expected Qualcomm 17cb:0113 PCI bridge')
    if field(0x0c, 0x7f0000) != 0x010000 or not field(4, 1 << 20):
        raise ValueError('root bridge header or capability-list indicator invalid')
    p = field(0x34, 0xff)
    seen = set()
    xp = None
    standard_end = 0
    while p:
        check_ptr(p, 0x40, 0xfc)
        if p in seen or len(seen) >= STD_LIMIT or (xp is not None and xp < p < standard_end):
            raise ValueError('invalid or incomplete standard capability list')
        seen.add(p)
        word = read(p)
        capid = word & 0xff
        if capid in (0, 0xff):
            raise ValueError('invalid standard capability identifier')
        # Non-PCIe control/status upper halves may legitimately vary at runtime.
        mask = 0x01ffffff if capid == 0x10 else 0xffff
        observations.append((p, mask, word & mask))
        if capid == 0x10:
            flags = word >> 16
            version, kind = flags & 0xf, (flags >> 4) & 0xf
            if xp is not None or version not in (1, 2) or kind != 4:
                raise ValueError('duplicate, unknown or non-root PCIe capability')
            xp = p
            # Root Status ends at +0x23. In v2 Link Status 2 ends at +0x33;
            # if Slot Implemented is set, Slot Status 2 ends at +0x3b.
            standard_end = p + (0x24 if version == 1 else (0x3c if flags & 0x100 else 0x34))
            if standard_end > 0x100 or any(p < old < standard_end for old in seen):
                raise ValueError('overlapping/truncated PCIe root-port capability')
        p = (word >> 8) & 0xff
    if xp is None:
        raise ValueError('PCIe root-port capability missing')
    lnkcap = field(xp + 0x0c)
    if lnkcap & 0xf != 3 or (lnkcap >> 4) & 0x3f != 2:
        raise ValueError('unexpected maximum link speed or width')

    p, sec = 0x100, None
    visited, headers = set(), []
    while p:
        check_ptr(p, 0x100, 0xffc)
        if p in visited or len(visited) >= EXT_LIMIT or (sec is not None and sec < p < sec + 0x10):
            raise ValueError('invalid, overlapping or incomplete extended capability list')
        visited.add(p)
        h = field(p)
        headers.append({'offset': p, 'raw': h})
        if h == 0 and p == 0x100:
            break  # architected empty extended list; later linked zero is NOT empty
        capid, ver, nxt = h & 0xffff, (h >> 16) & 0xf, h >> 20
        if capid in (0, 0xffff) or not ver:
            raise ValueError('invalid linked extended capability header')
        if capid == 0x19:
            if sec is not None or ver != 1 or p > 0xff0 or any(p < old < p + 0x10 for old in visited):
                raise ValueError('duplicate, overlapping, truncated or unsupported SECPCI capability')
            sec = p
        p = nxt  # inspect raw low pointer bits; do not mask malformed alignment

    raw, lanes = None, []
    if sec is not None:
        raw = read(sec + 0x0c)
        lanes = [raw & 0xffff, raw >> 16]
        if 0xffff in lanes:
            raise ValueError('unavailable lane control word')
    # Recheck the original layout only, never chase a changed pointer. Finish
    # with identity (first observation) to catch hot removal/replacement.
    for off, mask, previous in reversed(observations):
        if (read(off) & mask) != previous:
            raise ValueError(f'root identity or capability layout changed at {off:#x}')
    return {'status': 'present' if sec is not None else 'absent', 'offset': sec,
            'raw': raw, 'lanes': lanes, 'pcie_offset': xp, 'link_capabilities': lnkcap,
            'extended_headers': headers}
