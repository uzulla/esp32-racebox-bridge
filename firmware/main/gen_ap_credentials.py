#!/usr/bin/env python3
"""Writes the soft-AP SSID and password as a C header. Run on every build (see CMakeLists.txt),
so each firmware image gets new values. The header goes to the build directory, never into git."""
import secrets
import sys

# No look-alike characters (0/O, 1/l/I): the password is typed in by hand.
ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZabcdefghijkmnpqrstuvwxyz"

ssid = "RCBXBRD-" + "".join(secrets.choice("23456789ABCDEFGHJKLMNPQRSTUVWXYZ") for _ in range(6))
password = "".join(secrets.choice(ALPHABET) for _ in range(12))

with open(sys.argv[1], "w") as f:
    f.write(f'#define RACEBOX_AP_SSID "{ssid}"\n#define RACEBOX_AP_PASSWORD "{password}"\n')
print(f"soft-AP: SSID {ssid}  password {password}")
