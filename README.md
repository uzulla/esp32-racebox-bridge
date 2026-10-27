# esp32-racebox

RaceBox Mini --BLE--> ESP32-S3 / ESP32-C6 --Wi-Fi UDP broadcast (JSON)--> racebox-dump (Go)

## Firmware (`firmware/`, ESP-IDF v6.1)

Targets: ESP32-S3 and ESP32-C6 (Waveshare ESP32-C6-GEEK checked: BLE link at 25 packets/s). They share
`sdkconfig.defaults`, which does not set `CONFIG_IDF_TARGET`; the target comes from `IDF_TARGET`.
Each target gets its own `build-<target>/` and `sdkconfig.<target>`.

With [mise](https://mise.jdx.dev) (builds and flashes on the host that has ESP-IDF and the board, `wvx` by default):

```sh
mise run fw:flash                    # TARGET=esp32c6 by default
TARGET=esp32s3 mise run fw:flash
mise run ble:status                  # BLE config service, from this machine
mise run ble:wifi-set
mise run udp:dump                    # watch the UDP stream (-- -raw for JSON)
```

`BUILD_HOST`, `SERIAL_PORT` and `TARGET` can be overridden from the environment. Or by hand:

```sh
. ~/esp/esp-idf-v6.1/export.sh
cd firmware
idf.py set-target esp32s3   # or esp32c6
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
idf.py menuconfig            # "RaceBox bridge": UDP port, device name filter, min CPU MHz

# parser unit tests on the host (uses the Unity bundled with ESP-IDF)
cmake -S test/host -B test/host/build && cmake --build test/host/build && test/host/build/test_racebox
```

- Scans for a device whose name starts with `RaceBox `, connects, and subscribes to the
  UART TX characteristic (`6E400003-…`). Reconnects automatically after a disconnect.
- Notifications go straight to a data task that reassembles packets and broadcasts each one
  as soon as it is complete (no polling).
- UDP port 5005 by default, sent to the subnet broadcast address with `MSG_DONTWAIT`: a datagram
  that can't go out immediately is dropped rather than delayed.
- Power: dynamic frequency scaling between 80 MHz and 160 MHz (`CONFIG_PM_ENABLE`).
- Partition table keeps NVS at 0x9000 and has two OTA slots.

### Wi-Fi credentials

Credentials are not in the source or the firmware image. They are saved to the ESP32's NVS
over USB serial and survive reflashing (`idf.py erase-flash` removes them):

```sh
uv run tools/set-wifi.py              # prompts for SSID and password (hidden)
uv run tools/set-wifi.py --clear
```

Under the hood this sends the line `wifi-set<TAB>ssid<TAB>password` (or `wifi-clear`) to the
serial console. `ble-disconnect` drops the BLE link to exercise the reconnect path.

- Scans for a device whose name starts with `RaceBox `, connects, and subscribes to the
  UART TX characteristic (`6E400003-…`). Reconnects automatically after a disconnect.
- To pin a specific device, build with `-DRACEBOX_FILTER='"1234567890"'` (substring of the name).
- UDP port: 5005 (`-DUDP_PORT=...`), sent to the subnet broadcast address.

### BLE config service

For `CONFIG_RACEBOX_CONFIG_WINDOW_S` (60) seconds after boot the ESP32 also advertises as
`ESP32-RaceBox-Bridge-XXXX` with a config service; the RaceBox link and UDP keep running meanwhile.
A client that connects within the window may stay until it disconnects. The link is encrypted
(Just Works pairing, no bond stored), so macOS asks to confirm ("接続") on each connection.

```sh
uv run tools/ble-config.py status      # RaceBox link, configured SSID, Wi-Fi connected?, IP
uv run tools/ble-config.py wifi-set    # prompts for SSID/password, saved to NVS and applied
```

| Characteristic | UUID | |
|---|---|---|
| service | `9b3c0001-4e5f-4a2b-8c6d-2f1e0a7b5c3d` | |
| status | `9b3c0002-…` | read: `{"racebox":"connected","racebox_device":"RaceBox Mini …","wifi_ssid":"…","wifi_connected":true,"ip":"192.168.11.38"}` (`ip` is `null` while disconnected) |
| wifi | `9b3c0003-…` | write with response: `<ssid>\t<password>` (SSID 1–32 bytes, password ≤ 64); rejected with ATT error 0x13 if invalid |

If the macOS pairing dialog shows an old device name after the firmware renamed the device:
macOS records the device in `/Library/Bluetooth/com.apple.MobileBluetooth.ledevices.paired.db`
even without a stored bond, and it is not listed in System Settings > Bluetooth. With Bluetooth
turned off, delete its row (the `Uuid` is the identifier `ble-config.py` prints) and restart
`bluetoothd`:

```sh
DB=/Library/Bluetooth/com.apple.MobileBluetooth.ledevices.paired.db
U=<uuid>
sudo sqlite3 $DB "DELETE FROM PairedDevices WHERE Uuid='$U'; DELETE FROM CustomProperties WHERE Uuid='$U';"
sudo pkill bluetoothd
```

## UDP payload

One JSON object per datagram. Datagrams are never fragmented: a racebox message is at most
~730 bytes even with every field at its widest (checked by a unit test), and the firmware drops
anything over 1000 bytes instead of sending it (a 1500-byte MTU allows 1472; the rest is headroom). Lost datagrams are
not retransmitted; freshness matters more than completeness.

`type: "racebox"`: one per RaceBox Data Message (up to 25/s).

```json
{"type":"racebox","seq":7,"device":"RaceBox Mini 0123456789","itow":118286240,
 "time":"2022-01-10T08:51:08.239971626Z","time_valid":true,"time_confirmed":true,"time_acc_ns":25,
 "fix":3,"fix_ok":true,"sats":11,"lat":42.6719035,"lon":23.2887238,"latlon_valid":true,
 "alt_wgs_m":625.761,"alt_msl_m":590.095,"h_acc_m":0.924,"v_acc_m":1.836,
 "speed_mps":0.035,"speed_acc_mps":0.208,"heading_deg":0.00000,"heading_acc_deg":145.26856,"heading_valid":false,
 "pdop":3.00,"battery":{"percent":89,"charging":false},
 "g":{"x":-0.003,"y":0.113,"z":0.974},"gyro_dps":{"x":-2.09,"y":0.86,"z":-0.04}}
```

- `time` is `null` unless the date, time and "fully resolved" validity bits are all set.
  `time_confirmed` is true when the receiver also confirmed UTC date and time (Date/Time Flags bits 5–7).
- `fix_ok`: fix status is 3D and the "valid fix" flag is set.
- `battery` is `{"input_voltage": V}` on a RaceBox Micro.
- `seq` increments per datagram and restarts from 0 on reboot; gaps mean lost datagrams.

`type: "status"`: once per second (BLE state, packet/error counters, Wi-Fi RSSI).

## Client (`client/`)

```sh
cd client && go build -o racebox-dump . && ./racebox-dump        # summary per line
./racebox-dump -raw                                              # raw JSON
```
