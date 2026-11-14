#!/usr/bin/env python3
"""Read Android's PCI/MHI, power-supply and I2C driver identities through ADB.

Run while Android is already booted and root access for shell is available.
This neither starts iOS nor repairs its Wi-Fi/battery drivers. It reads only
the sysfs attributes listed below, never raw buses, registers or partitions.
The JSON is private diagnostic input and must be saved outside this checkout.
Device serials, network addresses, SSIDs, credentials and battery serial
numbers are intentionally excluded. No general getprop/dumpsys/dmesg dump.
"""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[2]
SCHEMA = "ace5-connectivity-power-inventory/v1"
HEADER = "ACE5_SYSFS_INVENTORY_V1"
MAX_OUTPUT = 4 * 1024 * 1024
MAX_VALUE = 4096
FIELDS = {
    "pci": ("vendor", "device", "subsystem_vendor", "subsystem_device",
            "class", "revision", "driver"),
    "mhi": ("modalias", "driver"),
    "power_supply": ("type", "present", "online", "status", "health",
                     "capacity", "voltage_now", "current_now", "temp",
                     "charge_now", "charge_full", "charge_full_design",
                     "cycle_count", "technology", "model_name", "manufacturer"),
    "i2c": ("name", "driver", "of_compatible"),
}
PATHS = {
    "pci": "/sys/bus/pci/devices/*",
    "mhi": "/sys/bus/mhi/devices/*",
    "power_supply": "/sys/class/power_supply/*",
    "i2c": "/sys/bus/i2c/devices/*",
}
NUMERIC = {"present", "online", "capacity", "voltage_now", "current_now",
           "temp", "charge_now", "charge_full", "charge_full_design", "cycle_count"}
NODE_RE = re.compile(r"[A-Za-z0-9_.:+-]{1,200}\Z")


def shell_script():
    # All remote paths and field names are constants. User arguments are never
    # interpolated into shell code. 'head' bounds even an unexpected attribute.
    script = '''
[ "$(id -u)" = 0 ] || exit 71
command -v head >/dev/null 2>&1 || exit 72
command -v od >/dev/null 2>&1 || exit 72
command -v tr >/dev/null 2>&1 || exit 72
command -v readlink >/dev/null 2>&1 || exit 72
printf 'ACE5_SYSFS_INVENTORY_V1\\n'
read_field() {
    group=$1
    node=$2
    field=$3
    path=$4
    if [ "$field" = driver ]; then
        if [ ! -L "$path" ]; then
            printf 'E\\t%s\\t%s\\t%s\\tabsent\\n' "$group" "$node" "$field"
            return
        fi
        value=$(readlink -f "$path" 2>/dev/null) || {
            printf 'E\\t%s\\t%s\\t%s\\tunreadable\\n' "$group" "$node" "$field"
            return
        }
        hex=$(printf '%s' "$value" | od -An -v -tx1 | tr -d ' \\n')
    else
        if [ ! -e "$path" ]; then
            printf 'E\\t%s\\t%s\\t%s\\tabsent\\n' "$group" "$node" "$field"
            return
        fi
        if [ ! -r "$path" ]; then
            printf 'E\\t%s\\t%s\\t%s\\tunreadable\\n' "$group" "$node" "$field"
            return
        fi
        # The status suffix survives the pipeline and detects a failed sysfs read.
        hex=$({ head -c 4097 "$path" 2>/dev/null; printf '\\nREAD_STATUS:%s' "$?"; } | od -An -v -tx1 | tr -d ' \\n')
    fi
    printf 'R\\t%s\\t%s\\t%s\\t%s\\n' "$group" "$node" "$field" "$hex"
}
'''
    for group, fields in FIELDS.items():
        script += f"count=0\nfor directory in {PATHS[group]}; do\n"
        script += '    [ -d "$directory" ] || continue\n'
        script += '    count=$((count + 1))\n    [ "$count" -le 256 ] || exit 73\n'
        script += '    node=${directory##*/}\n'
        for field in fields:
            filename = "of_node/compatible" if field == "of_compatible" else field
            script += f'    read_field {group} "$node" {field} "$directory/{filename}"\n'
        script += "done\n"
    script += "printf 'END\\n'\n"
    return script


def decode_value(group, field, encoded):
    if len(encoded) > (MAX_VALUE + 64) * 2 or not re.fullmatch(r"(?:[0-9a-f]{2})*", encoded):
        raise ValueError("Malformed or oversized sysfs value (content withheld)")
    raw = bytes.fromhex(encoded)
    if field != "driver":
        content, separator, status = raw.rpartition(b"\nREAD_STATUS:")
        if not separator or not re.fullmatch(rb"[0-9]{1,3}", status):
            raise ValueError("Missing sysfs read status")
        if status != b"0":
            return None, "read_failed"
        raw = content
    if len(raw) > MAX_VALUE:
        return None, "too_long"
    value = raw.decode("utf-8", errors="strict").rstrip("\r\n")
    if field == "of_compatible":
        parts = value.rstrip("\0").split("\0") if value else []
        if any(not re.fullmatch(r"[A-Za-z0-9,._+-]+", part) for part in parts):
            raise ValueError("Malformed compatible string (content withheld)")
        return parts, None
    if any(ord(char) < 32 or ord(char) == 127 for char in value):
        raise ValueError("Control characters in sysfs value (content withheld)")
    if field == "driver" and not re.fullmatch(r"/sys/(?:bus|devices|class)/[A-Za-z0-9_/.:+-]+", value):
        raise ValueError("Unexpected driver link (content withheld)")
    if group == "pci" and field != "driver":
        if not re.fullmatch(r"0x[0-9a-fA-F]{1,8}", value):
            raise ValueError("Malformed PCI identifier (content withheld)")
        return value.lower(), None
    if field in NUMERIC:
        if not re.fullmatch(r"-?[0-9]{1,16}", value):
            return None, "non_numeric"
        return int(value), None
    return value, None


def parse_inventory(payload):
    if len(payload) > MAX_OUTPUT:
        raise ValueError("Inventory exceeds the size limit")
    lines = payload.decode("ascii", errors="strict").splitlines()
    if not lines or lines[0] != HEADER or lines[-1] != "END":
        raise ValueError("Incomplete or unexpected inventory response")
    groups = {group: {} for group in FIELDS}
    seen = set()
    for line in lines[1:-1]:
        parts = line.split("\t")
        if len(parts) != 5:
            raise ValueError("Malformed inventory record")
        kind, group, node, field, value = parts
        if group not in FIELDS or field not in FIELDS[group] or not NODE_RE.fullmatch(node):
            raise ValueError("Inventory contains an unapproved field or node")
        key = (group, node, field)
        if key in seen:
            raise ValueError("Duplicate inventory record")
        seen.add(key)
        item = groups[group].setdefault(node, {"node": node, "values": {}, "unavailable": {}})
        if len(groups[group]) > 256:
            raise ValueError("Too many inventory nodes")
        if kind == "E":
            if value not in {"absent", "unreadable"}:
                raise ValueError("Unexpected read error")
            item["unavailable"][field] = value
        elif kind == "R":
            decoded, error = decode_value(group, field, value)
            if error:
                item["unavailable"][field] = error
            else:
                item["values"][field] = decoded
        else:
            raise ValueError("Unknown inventory record type")
    # Completeness is checked per observed node; zero nodes is a valid result.
    for group, nodes in groups.items():
        for item in nodes.values():
            if set(item["values"]) | set(item["unavailable"]) != set(FIELDS[group]):
                raise ValueError("Incomplete inventory node")
    return {group: list(nodes.values()) for group, nodes in groups.items()}


def validate_output(path, root=ROOT):
    result = Path(path).expanduser().resolve()
    if result.is_relative_to(root.resolve()):
        raise ValueError("Save this private report outside the source checkout")
    if result.exists():
        raise ValueError("Output already exists; refusing to overwrite it")
    if not result.parent.is_dir():
        raise ValueError("Output parent directory must already exist")
    return result


def run_adb(adb, arguments, timeout):
    try:
        result = subprocess.run([adb, *arguments], capture_output=True, timeout=timeout)
    except (OSError, subprocess.TimeoutExpired) as exc:
        # ADB errors can include serials or host paths. Do not print raw output.
        raise ValueError("ADB could not complete the read-only request") from exc
    if result.returncode:
        raise ValueError("ADB/root read failed; keep Android booted and permit shell root access")
    return result.stdout


def select_device(adb, serial):
    if serial:
        if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,200}", serial):
            raise ValueError("Invalid ADB device selector")
        return serial
    devices = []
    for line in run_adb(adb, ["devices"], 15).decode("utf-8", errors="replace").splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[1] == "device":
            devices.append(parts[0])
    if len(devices) != 1:
        raise ValueError("Exactly one authorized Android device is required, or supply --serial")
    if not re.fullmatch(r"[A-Za-z0-9_.:-]{1,200}", devices[0]):
        raise ValueError("Invalid ADB device selector")
    return devices[0]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--adb", default="adb", help="ADB executable path")
    parser.add_argument("--serial", help="Optional ADB selector; never saved in the report")
    parser.add_argument("--output", required=True, type=Path, help="New private JSON path outside this checkout")
    args = parser.parse_args(argv)
    try:
        output = validate_output(args.output)
        adb = shutil.which(args.adb)
        if not adb:
            raise ValueError("ADB executable was not found")
        serial = select_device(adb, args.serial)
        data = run_adb(adb, ["-s", serial, "exec-out", "su", "-c", shell_script()], 60)
        report = {
            "schema": SCHEMA,
            "captured_utc": datetime.now(timezone.utc).isoformat(),
            "method": "Android root; allowlisted sysfs reads only",
            "port_status": "Inventory only; does not establish iOS Wi-Fi or battery support",
            "privacy": "Private diagnostic input. No device/battery serial, network address, SSID or credential collected.",
            "units": {"voltage_now": "microvolts", "current_now": "microamps; sign is driver-defined",
                      "temp": "tenths of degrees Celsius", "capacity": "percent",
                      "charge_now": "microamp-hours", "charge_full": "microamp-hours",
                      "charge_full_design": "microamp-hours"},
            "inventory": parse_inventory(data),
        }
        # Exclusive creation also protects against a file appearing during ADB.
        with output.open("x", encoding="utf-8", newline="\n") as stream:
            json.dump(report, stream, ensure_ascii=False, indent=2)
            stream.write("\n")
    except (ValueError, UnicodeError, OSError) as exc:
        print(f"Inventory not saved: {exc}", file=sys.stderr)
        return 1
    print("Saved private read-only inventory. Wi-Fi and battery driver support remains unverified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
