#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["bleak"]
# ///
"""Talk to the ESP32's BLE config service (advertised for 60 s after boot).

    uv run tools/ble-config.py status      # RaceBox link, Wi-Fi SSID, connected?, IP
    uv run tools/ble-config.py wifi-set    # prompts for SSID and password, saved to NVS

The link is encrypted (Just Works pairing, nothing stored): the device asks
the OS to pair as soon as it connects.
"""
import argparse
import asyncio
import getpass
import json
import sys

from bleak import BleakClient, BleakScanner
from bleak.exc import BleakError

SERVICE = "9b3c0001-4e5f-4a2b-8c6d-2f1e0a7b5c3d"
STATUS = "9b3c0002-4e5f-4a2b-8c6d-2f1e0a7b5c3d"
WIFI = "9b3c0003-4e5f-4a2b-8c6d-2f1e0a7b5c3d"


async def find(name, timeout):
    print(f"scanning for up to {timeout:.0f} s (power-cycle the ESP32 if its 60 s window has passed)...",
          file=sys.stderr)
    found = {}

    def match(d, adv):
        if SERVICE not in [u.lower() for u in adv.service_uuids]:
            return False
        # Prefer the advertised name (from the scan response, which may not have
        # arrived yet): macOS may show a name cached from earlier firmware.
        found["name"] = adv.local_name or d.name
        return name is None or adv.local_name == name

    dev = await BleakScanner.find_device_by_filter(match, timeout=timeout)
    if dev is None:
        sys.exit("device not found")
    print(f"found {found['name']} ({dev.address})", file=sys.stderr)
    return dev


async def when_encrypted(op, timeout=30):
    """Run op, retrying while pairing is in progress (macOS asks the user to confirm it)."""
    loop = asyncio.get_running_loop()
    deadline = loop.time() + timeout
    while True:
        try:
            return await op()
        except BleakError as e:
            if "Encryption" not in str(e) or loop.time() > deadline:
                raise
            await asyncio.sleep(0.5)


async def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("command", choices=["status", "wifi-set"])
    ap.add_argument("--name", help="advertised name, e.g. ESP32-RaceBox-Bridge-06FA (default: first found)")
    ap.add_argument("--timeout", type=float, default=30)
    args = ap.parse_args()

    if args.command == "wifi-set":
        ssid = input("SSID: ")
        password = getpass.getpass("Password: ")
        if not ssid or "\t" in ssid + password:
            print("SSID must be non-empty; tabs are not allowed", file=sys.stderr)
            return 1

    dev = await find(args.name, args.timeout)
    async with BleakClient(dev) as client:
        if args.command == "status":
            data = await when_encrypted(lambda: client.read_gatt_char(STATUS))
            print(json.dumps(json.loads(data), indent=2, ensure_ascii=False))
        else:
            try:
                await when_encrypted(
                    lambda: client.write_gatt_char(WIFI, f"{ssid}\t{password}".encode(), response=True))
            except BleakError as e:
                print(f"rejected by the device: {e}", file=sys.stderr)
                return 1
            print("OK wifi saved")
    return 0


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
