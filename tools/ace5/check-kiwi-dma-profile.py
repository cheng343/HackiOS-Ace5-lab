#!/usr/bin/env python3
"""Offline check of the Giulia RC0/KIWI device-tree configuration, not a DMA grant."""
import argparse
import json
from pathlib import Path
import struct
import sys

from share import fdt_properties


MAX_DTB = 8 * 1024 * 1024
RC = '/soc/qcom,pcie@1c00000'
SMMU = '/soc/apps-smmu@15000000'
RP = RC + '/pcie0_rp'
EP = RP + '/cnss_pci0'
GROUP = EP + '/cnss_pci_iommu_group0'


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def checked_tree(data):
    """Validate the entire extent before letting the common FDT reader decode the tree."""
    require(40 <= len(data) <= MAX_DTB, 'unexpected DTB file size')
    magic, total, start, strings, reserve, version, compat, _, slen, size = struct.unpack_from('>10I', data)
    require(magic == 0xd00dfeed and total == len(data) and version == 17 and compat <= 17,
            'invalid flattened-device-tree header or unsupported version')
    require(40 <= reserve < start and reserve % 8 == 0 and start % 4 == 0 and size % 4 == 0 and
            size >= 16 and start + size <= strings and slen > 0 and strings + slen <= total,
            'overlapping, misaligned or out-of-bounds FDT blocks')
    for pos in range(reserve, start - 15, 16):
        addr, length = struct.unpack_from('>QQ', data, pos)
        if addr == length == 0:
            break
        require(length and addr + length <= 1 << 64, 'invalid reserved memory extent')
    else:
        raise ValueError('unterminated reserved-memory map')
    # share.fdt_properties checks actual tokens, names, duplicate nodes and properties.
    # Additionally reject a hidden second tree/END, missing final END, excessive depth,
    # out-of-bounds padding, or an empty child masquerading as another absolute path.
    off, limit, depth, roots, count = start, start + size, 0, 0, 0
    while off + 4 <= limit:
        token, = struct.unpack_from('>I', data, off)
        off += 4
        if token == 1:
            end = data.find(b'\0', off, min(limit, off+256))
            require(end >= 0, 'unterminated or oversized node name')
            if depth == 0:
                roots += 1
                require(roots == 1 and off == end, 'invalid or duplicated root')
            else:
                require(end > off and data[off:end] not in (b'.',b'..'), 'invalid child node name')
            depth += 1
            count += 1
            require(depth <= 64 and count <= 65536, 'unreasonable FDT tree depth or node count')
            padded = (end+4) & ~3
            require(padded <= limit, 'invalid node name padding')
            off = padded
        elif token == 2:
            require(depth > 0, 'unbalanced end-node')
            depth -= 1
        elif token == 3:
            require(depth and off+8 <= limit, 'truncated or orphaned FDT property')
            length, key = struct.unpack_from('>II', data, off)
            off += 8
            end = off + length
            padded = (end+3) & ~3
            require(key < slen and data.find(b'\0',strings+key,min(strings+slen,strings+key+256)) > strings+key,
                    'invalid property name offset or length')
            # The archived bootloader left a nonzero byte in bootargs alignment padding.
            # Validate the extent and skip the padding; it is not part of the property.
            require(padded <= limit, 'truncated or misaligned FDT property')
            off = padded
        elif token == 4:  # FDT_NOP
            continue
        elif token == 9:  # FDT_END
            require(roots == 1 and depth == 0 and off == limit, 'early or incomplete FDT end')
            return fdt_properties(data)
        else:
            raise ValueError('unknown FDT token')
    raise ValueError('missing FDT end')


def cells(properties, name, n=None):
    value = properties.get(name)
    require(value is not None and len(value) and len(value) % 4 == 0 and
            (n is None or len(value) == 4*n), f'missing or malformed {name}')
    return struct.unpack(f'>{len(value)//4}I', value)


def strings(properties, name):
    value = properties.get(name)
    require(value and value.endswith(b'\0'), f'missing or malformed {name}')
    parts = value[:-1].split(b'\0')
    require(all(parts), f'empty entry in {name}')
    return [p.decode('ascii') for p in parts]


def phandles(nodes):
    result = {}
    for path, props in nodes.items():
        ids = [cells(props,k,1)[0] for k in ('phandle','linux,phandle') if k in props]
        if ids:
            require(ids[0] not in (0,0xffffffff) and len(set(ids)) == 1 and ids[0] not in result,
                    'invalid, conflicting or duplicate phandle')
            result[ids[0]] = path
    return result


def read_profile(data):
    nodes = checked_tree(data)
    for path in ('/', '/soc', RC, RP, EP, SMMU, GROUP):
        require(path in nodes, 'missing required node ' + path)
        require(nodes[path].get('status',b'okay\0') in (b'okay\0',b'ok\0'), 'disabled node ' + path)
    model = strings(nodes['/'],'model')
    require(len(model) == 1 and model[0].lower().endswith(',giulia') and
            'qcom,pineapple' in strings(nodes['/'],'compatible'), 'not a Giulia/Pineapple board')
    root, provider, group = nodes[RC], nodes[SMMU], nodes[GROUP]
    for path, address_cells, size_cells in (('/',2,2),('/soc',1,1),(RC,3,2),(RP,5,0)):
        require(cells(nodes[path],'#address-cells',1) == (address_cells,) and
                cells(nodes[path],'#size-cells',1) == (size_cells,), 'unexpected bus cell widths at ' + path)
    # A missing ranges is not an identity mapping. Stock /soc explicitly maps
    # its 32-bit space with a zero-based 1+2+1-cell range.
    require(nodes['/soc'].get('ranges') in (b'',struct.pack('>4I',0,0,0,0xffffffff)),
            'unexpected SoC register address translation')
    # These are local OF addresses, not endpoint requester IDs. The endpoint
    # RID comes from the validated RC iommu-map and the reference Android BDF.
    require(cells(nodes[RP],'reg',5) == (0,)*5 and cells(nodes[EP],'reg',5) == (0,)*5,
            'unexpected local root-port or endpoint bus address')
    for props, comp, base, first_name in ((root,'qcom,pci-msm',0x1c00000,'parf'),
                                          (provider,'qcom,qsmmu-v500',0x15000000,'base')):
        reg = cells(props,'reg')
        names = strings(props,'reg-names')
        require(comp in strings(props,'compatible') and len(reg) == 2*len(names) and
                len(set(names)) == len(names) and names[0] == first_name and reg[0] == base and reg[1] > 0,
                'unexpected controller identity or register region')
    require(cells(root,'linux,pci-domain',1) == (0,) and strings(root,'device_type') == ['pci'] and
            root.get('dma-coherent') == b'' and cells(root,'qcom,smmu-sid-base',1) == (0x1400,),
            'not the coherent native RC0/SID base')
    if 'iommu-map-mask' in root:
        require(cells(root,'iommu-map-mask',1) == (0xffffffff,), 'unexpected requester-ID mask')
    require(cells(provider,'#iommu-cells',1) == (2,), 'unexpected apps-SMMU provider')
    # Downstream PCIe parses fixed {BDF, phandle, SID, length} rows. The SMMU
    # provider's #iommu-cells is not an extra stride cell in this table.
    ids = phandles(nodes)
    table = cells(root,'iommu-map',8)
    routes = {}
    for rid,handle,sid,count in (table[:4],table[4:]):
        require(count == 1 and rid not in routes and ids.get(handle) == SMMU, 'ambiguous requester/provider mapping')
        routes[rid] = (sid,handle)
    require({r:s for r,(s,_) in routes.items()} == {0:0x1400,0x100:0x1401},
            'unexpected requester/stream assignment')
    handle, = cells(nodes[EP],'qcom,iommu-group',1)
    require(ids.get(handle) == GROUP and strings(group,'qcom,iommu-dma') == ['fastmap'] and
            strings(group,'qcom,iommu-pagetable') == ['coherent'], 'unexpected endpoint IOMMU group or mode')
    # A path-local group does not by itself prevent another endpoint referring
    # to it. The verified Giulia profile gives this CNSS group a single consumer.
    encoded = struct.pack('>I', handle)
    require(not any(path != EP and props.get('qcom,iommu-group',b'')[:4] == encoded
                    for path, props in nodes.items()), 'CNSS group shared by another endpoint')
    first, length = cells(group,'qcom,iommu-dma-addr-pool',2)
    gfirst, glength = cells(group,'qcom,iommu-geometry',2)
    require((first,length) == (0x98000000,0x18000000) and
            (gfirst,glength) == (first,0x18010000) and cells(group,'qcom,iommu-msi-size',1) == (0x1000,),
            'unexpected CNSS DMA pool, geometry or MSI size')
    # Never expose the geometry tail as an allocatable payload pool.
    return {'status':'configuration_only','dma_authorized':False,
            'root':{'rid':0,'sid':routes[0][0]},'endpoint':{'rid':0x100,'sid':routes[0x100][0]},
            'provider':{'path':SMMU,'phandle':routes[0][1]},
            'group':{'path':GROUP,'phandle':handle,'dma_type':'fastmap'},
            'pool':{'first':first,'last':first+length-1,'size':length},
            'geometry':{'first':gfirst,'last':gfirst+glength-1,'size':glength}}


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('dtb',type=Path,help='saved Android flattened device tree (no device access)')
    args = ap.parse_args()
    try:
        with args.dtb.open('rb') as stream:
            data = stream.read(MAX_DTB+1)
        profile = read_profile(data)
    except (OSError, ValueError, RuntimeError, struct.error) as exc:
        print(f'DMA profile rejected: {exc}',file=sys.stderr)
        return 1
    print(json.dumps(profile,sort_keys=True))
    return 0


if __name__ == '__main__':
    sys.exit(main())
