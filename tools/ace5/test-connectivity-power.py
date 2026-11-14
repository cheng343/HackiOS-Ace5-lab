#!/usr/bin/env python3
"""Offline inventory parser/privacy checks; never invokes ADB or a compiler."""
import importlib.util
import contextlib
import io
import json
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("inventory", Path(__file__).with_name("inspect-connectivity-power.py"))
inventory = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inventory)


def record(group, node, field, value, status=0):
    if isinstance(value, str):
        value = value.encode()
    if field != "driver":
        value += b"\nREAD_STATUS:" + str(status).encode()
    return f"R\t{group}\t{node}\t{field}\t{value.hex()}"


def response(records):
    return ("\n".join([inventory.HEADER, *records, "END"]) + "\n").encode()


def node_records(group, node, values):
    return [record(group, node, field, values[field]) if field in values else
            f"E\t{group}\t{node}\t{field}\tabsent" for field in inventory.FIELDS[group]]


class InventoryTests(unittest.TestCase):
    def test_realistic_data_and_missing_attributes(self):
        records = node_records("pci", "0000:01:00.0", {"vendor": "0x17CB\n", "device": "0x1107\n",
            "driver": "/sys/bus/pci/drivers/cnss_pci"})
        records += node_records("power_supply", "battery", {"capacity": "37\n", "current_now": "-321000\n",
            "temp": "287\n", "status": "Discharging\n", "model_name": "example-gauge\n"})
        records += node_records("i2c", "3-0055", {"name": "example_gauge\n",
            "of_compatible": b"vendor,example\x00vendor,fallback\x00"})
        parsed = inventory.parse_inventory(response(records))
        self.assertEqual(parsed["pci"][0]["values"]["vendor"], "0x17cb")
        self.assertEqual(parsed["power_supply"][0]["values"]["current_now"], -321000)
        self.assertEqual(parsed["power_supply"][0]["values"]["temp"], 287)
        self.assertEqual(parsed["i2c"][0]["values"]["of_compatible"], ["vendor,example", "vendor,fallback"])
        self.assertEqual(parsed["mhi"], [])
        self.assertEqual(parsed["pci"][0]["unavailable"]["revision"], "absent")

    def test_sysfs_failure_and_bounds(self):
        self.assertEqual(inventory.decode_value("power_supply", "capacity", b"\nREAD_STATUS:5".hex()),
                         (None, "read_failed"))
        self.assertEqual(inventory.decode_value("power_supply", "capacity", b"unknown\nREAD_STATUS:0".hex()),
                         (None, "non_numeric"))
        self.assertEqual(inventory.decode_value("power_supply", "model_name",
                         (b"x" * 4097 + b"\nREAD_STATUS:0").hex()), (None, "too_long"))
        with self.assertRaises(ValueError):
            inventory.parse_inventory(b"x" * (inventory.MAX_OUTPUT + 1))

    def test_reject_unapproved_or_incomplete_content(self):
        bad = [record("power_supply", "battery", "serial_number", "secret"),
               record("wifi", "wlan0", "ssid", "secret"),
               record("i2c", "../escape", "name", "gauge"),
               "R\tpci\t0000:01:00.0\tvendor\tzz",
               record("pci", "0000:01:00.0", "vendor", "0x17cb"),
               record("mhi", "mhi0", "driver", "/private/secret")]
        for line in bad:
            with self.subTest(line=line), self.assertRaises(ValueError):
                inventory.parse_inventory(response([line]))
        complete = node_records("mhi", "mhi0", {})
        with self.assertRaises(ValueError):
            inventory.parse_inventory(response(complete + complete))
        with self.assertRaises(ValueError):
            inventory.parse_inventory(response(complete).replace(b"END", b""))

    def test_output_private_and_exclusive(self):
        with tempfile.TemporaryDirectory() as temporary:
            base = Path(temporary)
            checkout = base / "repo"
            checkout.mkdir()
            with self.assertRaises(ValueError):
                inventory.validate_output(checkout / "private.json", checkout)
            target = base / "private.json"
            self.assertEqual(inventory.validate_output(target, checkout), target.resolve())
            target.write_text("keep", encoding="utf-8")
            with self.assertRaises(ValueError):
                inventory.validate_output(target, checkout)
            self.assertEqual(target.read_text(), "keep")

    def test_remote_script_is_fixed_and_read_only(self):
        script = inventory.shell_script()
        for forbidden in ("dumpsys", "getprop", "dmesg", "devmem", "i2cget", "i2cset", "reboot", "serial_number", "address", "uevent"):
            self.assertNotIn(forbidden, script)
        for path in inventory.PATHS.values():
            self.assertIn(path, script)
        self.assertNotIn("--serial", script)

    def test_device_selection_and_serial_not_saved(self):
        with mock.patch.object(inventory, "run_adb", return_value=b"List of devices attached\nexample-device\tdevice\n"):
            self.assertEqual(inventory.select_device("adb", None), "example-device")
        for listing in (b"", b"first\tdevice\nsecond\tdevice\n", b"example\tunauthorized\n"):
            with mock.patch.object(inventory, "run_adb", return_value=listing), self.assertRaises(ValueError):
                inventory.select_device("adb", None)
        with self.assertRaises(ValueError):
            inventory.select_device("adb", "x;reboot")

    def test_main_saves_no_selector_and_refuses_overwrite_before_adb(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "inventory.json"
            replies = [b"List of devices attached\nprivate-selector\tdevice\n", response([])]
            with mock.patch.object(inventory.shutil, "which", return_value="/fake/adb"), \
                 mock.patch.object(inventory, "run_adb", side_effect=replies) as adb, \
                 contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(inventory.main(["--output", str(target)]), 0)
                self.assertEqual(adb.call_args_list[1].args[1][:5],
                                 ["-s", "private-selector", "exec-out", "su", "-c"])
            saved = target.read_text(encoding="utf-8")
            self.assertNotIn("private-selector", saved)
            self.assertEqual(json.loads(saved)["inventory"], {group: [] for group in inventory.FIELDS})
            with mock.patch.object(inventory, "run_adb") as adb, contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(inventory.main(["--output", str(target)]), 1)
                adb.assert_not_called()
            self.assertEqual(target.read_text(encoding="utf-8"), saved)

    def test_main_does_not_save_incomplete_response(self):
        with tempfile.TemporaryDirectory() as temporary:
            target = Path(temporary) / "inventory.json"
            with mock.patch.object(inventory.shutil, "which", return_value="/fake/adb"), \
                 mock.patch.object(inventory, "run_adb", return_value=b"partial response"), \
                 contextlib.redirect_stderr(io.StringIO()):
                self.assertEqual(inventory.main(["--serial", "selected", "--output", str(target)]), 1)
            self.assertFalse(target.exists())


if __name__ == "__main__":
    unittest.main()
