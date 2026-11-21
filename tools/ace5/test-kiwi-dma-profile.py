# SPDX-License-Identifier: BSD-2-Clause
"""Black-box CLI regression: synthetic, minimal giulia FDT; never opens a device."""
import json
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = Path(__file__).resolve().with_name("check-kiwi-dma-profile.py")
RC = "/soc/qcom,pcie@1c00000"
SMMU = "/soc/apps-smmu@15000000"
RP = RC + "/pcie0_rp"
EP = RP + "/cnss_pci0"
GROUP = EP + "/cnss_pci_iommu_group0"


def cells(*v):
    return struct.pack(">" + "I" * len(v), *v)


def strings(*v):
    return b"".join(x.encode("ascii") + b"\x00" for x in v)


def fixture(ph=0x52, group=0x637):
    # Only the public board shape and numerical routing policy; no phone dump.
    return {
        "/": {"model": strings("Qualcomm Technologies, Inc. Pineapple MTP,giulia"),
              "compatible": strings("qcom,pineapple-mtp", "qcom,pineapple", "qcom,mtp"),
              "#address-cells": cells(2), "#size-cells": cells(2)},
        "/soc": {"compatible": strings("simple-bus"), "#address-cells": cells(1),
                 "#size-cells": cells(1), "ranges": b""},
        SMMU: {"compatible": strings("qcom,qsmmu-v500"), "#iommu-cells": cells(2),
               "reg": cells(0x15000000, 0x100000, 0x151fe000, 0x40),
               "reg-names": strings("base", "tcu-base"), "dma-coherent": b"", "phandle": cells(ph)},
        RC: {"compatible": strings("qcom,pci-msm"), "device_type": strings("pci"),
             "linux,pci-domain": cells(0), "#address-cells": cells(3), "#size-cells": cells(2),
             "reg": cells(0x1c00000, 0x3000), "reg-names": strings("parf"),
             "dma-coherent": b"", "qcom,smmu-sid-base": cells(0x1400),
             "iommu-map": cells(0, ph, 0x1400, 1, 0x100, ph, 0x1401, 1)},
        RP: {"reg": cells(0, 0, 0, 0, 0), "#address-cells": cells(5), "#size-cells": cells(0)},
        EP: {"phandle": cells(0x63a), "reg": cells(0, 0, 0, 0, 0), "qcom,iommu-group": cells(group),
             "#address-cells": cells(1), "#size-cells": cells(1)},
        GROUP: {"phandle": cells(group), "qcom,iommu-faults": strings("stall-disable", "HUPCF", "non-fatal"),
                "qcom,iommu-pagetable": strings("coherent"), "qcom,iommu-dma": strings("fastmap"),
                "qcom,iommu-dma-addr-pool": cells(0x98000000, 0x18000000),
                "qcom,iommu-geometry": cells(0x98000000, 0x18010000), "qcom,iommu-msi-size": cells(0x1000)},
    }


def flatten(nodes):
    names = bytearray()
    offsets = {}
    for key in sorted({k for props in nodes.values() for k in props}):
        offsets[key] = len(names)
        names.extend(strings(key))
    tree = bytearray()

    def emit(path):
        tree.extend(cells(1) + strings(path.rsplit("/", 1)[-1]))
        tree.extend(b"\0" * ((-len(tree)) % 4))
        for k, value in nodes[path].items():
            tree.extend(cells(3, len(value), offsets[k]) + value)
            tree.extend(b"\0" * ((-len(tree)) % 4))
        for child in sorted(nodes):
            if child != "/" and (child.rsplit("/", 1)[0] or "/") == path:
                emit(child)
        tree.extend(cells(2))

    emit("/")
    tree.extend(cells(9))
    strings_off = 56 + len(tree)
    return cells(0xd00dfeed, strings_off + len(names), 56, strings_off, 40,
                 17, 16, 0, len(names), len(tree)) + bytes(16) + tree + names


class KiwiDMAProfileCLI(unittest.TestCase):
    def run_blob(self, blob):
        with tempfile.TemporaryDirectory() as td:
            path = Path(td) / "generated.dtb"
            path.write_bytes(blob)
            return subprocess.run([sys.executable, "-B", str(CLI), str(path)], cwd=td,
                                  text=True, encoding="utf-8", errors="replace",
                                  capture_output=True, timeout=8)

    def test_configuration_only_and_phandle_resolution(self):
        # DT phandle numbers are local. Remapping both references and their
        # providers must leave the verified semantic identities unchanged.
        for ph, group in ((0x52, 0x637), (0x111, 0x222)):
            with self.subTest(ph=ph):
                nodes = fixture(ph, group)
                if ph == 0x111:
                    nodes[RC]["iommu-map-mask"] = cells(0xffffffff)
                    nodes[SMMU]["linux,phandle"] = cells(ph)
                p = self.run_blob(flatten(nodes))
                self.assertEqual(p.returncode, 0, p.stderr)
                v = json.loads(p.stdout)
                self.assertEqual(v["status"], "configuration_only")
                self.assertIs(v["dma_authorized"], False)
                for key, rid, sid in (("root", 0, 0x1400), ("endpoint", 0x100, 0x1401)):
                    self.assertEqual((v[key]["rid"], v[key]["sid"]), (rid, sid))
                self.assertEqual((v["provider"]["path"], v["provider"]["phandle"]), (SMMU, ph))
                self.assertEqual((v["group"]["path"], v["group"]["phandle"], v["group"]["dma_type"]),
                                 (GROUP, group, "fastmap"))
                for key, size in (("pool", 0x18000000), ("geometry", 0x18010000)):
                    self.assertEqual((v[key]["first"], v[key]["size"], v[key]["last"]),
                                     (0x98000000, size, 0x98000000 + size - 1))

    def reject(self, blob):
        p = self.run_blob(blob)
        self.assertNotEqual(p.returncode, 0, p.stdout)
        self.assertNotIn("configuration_only", p.stdout)

    def test_property_mutations(self):
        for label, node, k, value in (
            ("rid-sid swapped", RC, "iommu-map", cells(0, 0x52, 0x1401, 1, 0x100, 0x52, 0x1400, 1)),
            ("duplicate requester", RC, "iommu-map", cells(0, 0x52, 0x1400, 1, 0, 0x52, 0x1401, 1)),
            ("unknown provider", RC, "iommu-map", cells(0, 0x52, 0x1400, 1, 0x100, 0x77, 0x1401, 1)),
            ("range not singleton", RC, "iommu-map", cells(0, 0x52, 0x1400, 1, 0x100, 0x52, 0x1401, 2)),
            ("RID mask aliases", RC, "iommu-map-mask", cells(0xff)),
            ("parent uses a different address width", "/soc", "#address-cells", cells(2)),
            ("parent uses a different size width", "/soc", "#size-cells", cells(2)),
            ("first RC register is not PARF", RC, "reg-names", strings("phy")),
            # In this board's OF child record dev/function bits really are zero;
            # do not confuse that constraint with the endpoint RID in iommu-map.
            ("endpoint child device 1", EP, "reg", cells(0x800, 0, 0, 0, 0)),
            ("endpoint child function 1", EP, "reg", cells(0x100, 0, 0, 0, 0)),
            ("root port child device 1", RP, "reg", cells(0x800, 0, 0, 0, 0)),
            ("bad provider reg", SMMU, "reg", cells(0x15100000, 0x1000)),
            ("disabled provider", SMMU, "status", strings("disabled")),
            ("conflicting handle", SMMU, "linux,phandle", cells(0x77)),
            ("nonunique phandle", "/soc", "phandle", cells(0x52)),
            ("wrong group", EP, "qcom,iommu-group", cells(0x52)),
            ("unknown group", EP, "qcom,iommu-group", cells(0x123)),
            ("wrong pool start", GROUP, "qcom,iommu-dma-addr-pool", cells(0x98001000, 0x18000000)),
            ("wrong geometry size", GROUP, "qcom,iommu-geometry", cells(0x98000000, 0x18000000)),
            ("pool wraps", GROUP, "qcom,iommu-dma-addr-pool", cells(0xfffff000, 0x2000)),
            ("wrong DMA type", GROUP, "qcom,iommu-dma", strings("bypass")),
        ):
            with self.subTest(label=label):
                nodes = fixture()
                nodes[node][k] = value
                self.reject(flatten(nodes))

    def test_foreign_owner_and_malformed_tree(self):
        # A numerically identical group belonging to another device is not authorization.
        nodes = fixture()
        nodes["/soc/other-device"] = {}
        nodes["/soc/other-device/cnss_pci_iommu_group0"] = nodes.pop(GROUP)
        self.reject(flatten(nodes))
        # Keep the correct group in place: a second consumer elsewhere still
        # invalidates this board's single-consumer profile.
        shared = fixture()
        shared["/soc/other-device"] = {"qcom,iommu-group": cells(0x637)}
        self.reject(flatten(shared))
        good = flatten(fixture())
        end = struct.unpack_from(">I", good, 12)[0] - 4
        for label, blob in (
            ("bad magic", b"BAD!" + good[4:]), ("truncated", good[:48]),
            ("unknown structure token", good[:56] + cells(0x1234) + good[60:]),
            ("missing final end", good[:end] + cells(2) + good[end+4:]),
            ("overlapping blocks", good[:12] + cells(56) + good[16:]),
        ):
            with self.subTest(label=label):
                self.reject(blob)


if __name__ == "__main__":
    unittest.main()
