// Interface between the bridge (BLE -> Wi-Fi/UDP, in main/) and an optional frontend that shows
// its state (an LCD, LEDs, ...). The bridge knows nothing about any frontend: it fills a Status
// and hands it over. Exactly one frontend component is linked, chosen at build time with
// RACEBOX_FRONTEND (see firmware/CMakeLists.txt): frontend_none (no display) or frontend_lcd.
#pragma once

#include <stdint.h>

namespace bridge {

// Snapshot of the bridge and of the latest RaceBox data message.
struct Status {
  uint8_t ble;           // 0 scanning, 1 connecting, 2 connected
  bool configWindow;     // BLE config service is being advertised
  char device[64];       // RaceBox advertised name
  bool wifiConfigured;
  bool wifiUp;
  int8_t rssi;
  char ip[16];
  char ssid[33];
  uint32_t dataPackets;  // RaceBox data messages received since boot
  uint32_t udpErrors;

  // Latest RaceBox data message; meaningful when dataAtUs != 0.
  int64_t dataAtUs;      // esp_timer_get_time() of the last message, 0 = none yet
  uint8_t fix;           // 0 none, 2 2D, 3 3D
  bool fixOk;
  uint8_t sats;
  int32_t lat, lon;      // deg * 1e7
  int32_t altMm;         // MSL
  uint32_t hAccMm;
  int32_t speedMms;
  int32_t headingE5;     // deg * 1e5
  bool headingValid;
  int16_t gX, gY, gZ;    // milli-g
  int16_t rotX, rotY, rotZ;  // centi-deg/s
  int batteryPct;        // -1 when unknown (RaceBox Micro reports a voltage)
  bool charging;
};

// Called once from app_main before the bridge starts.
void frontendStart();
// Called whenever the bridge state or a RaceBox message changes (cheap; may be called 25+ times/s).
void frontendUpdate(const Status& s);

}  // namespace bridge
