#!/usr/bin/env python3
# /// script
# requires-python = ">=3.9"
# dependencies = ["pyserial"]
# ///
"""Save Wi-Fi credentials to the ESP32's NVS over USB serial.

The password is read with getpass and only sent to the device, so it never
ends up in the source tree or the firmware image.

    uv run tools/set-wifi.py [--port /dev/ttyACM0]
    uv run tools/set-wifi.py --clear
"""
import argparse
import getpass
import sys
import time

import serial


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", default="/dev/ttyACM0")
    ap.add_argument("--clear", action="store_true", help="erase the saved credentials")
    args = ap.parse_args()

    if args.clear:
        cmd = "wifi-clear"
    else:
        ssid = input("SSID: ")
        password = getpass.getpass("Password: ")
        if not ssid or "\t" in ssid + password or "\n" in ssid + password:
            print("SSID must be non-empty; tabs and newlines are not allowed", file=sys.stderr)
            return 1
        cmd = f"wifi-set\t{ssid}\t{password}"

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = 115200
    ser.timeout = 0.2
    # The USB-Serial/JTAG port resets the chip while RTS=1 and DTR=0. pyserial
    # applies DTR before RTS on open, so leave DTR asserted and only drop RTS.
    ser.rts = False
    try:
        ser.open()
    except serial.SerialException as e:
        print(f"{e}\nRun this on the machine the ESP32 is plugged into.", file=sys.stderr)
        return 1
    ser.reset_input_buffer()
    ser.write((cmd + "\n").encode())

    deadline = time.time() + 5
    while time.time() < deadline:
        line = ser.readline().decode(errors="replace").strip()
        if line.startswith("OK") or line.startswith("ERR"):
            print(line)
            return 0 if line.startswith("OK") else 1
    print("no response from the device", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
