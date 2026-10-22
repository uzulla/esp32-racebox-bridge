#include "racebox.h"

#include <stdio.h>
#include <string.h>

namespace racebox {

bool UbxParser::feed(uint8_t b) {
  switch (state_) {
    case State::Sync1:
      if (b == kSync1) state_ = State::Sync2;
      return false;
    case State::Sync2:
      if (b == kSync2) {
        state_ = State::Class;
        ckA_ = ckB_ = 0;
      } else if (b != kSync1) {
        state_ = State::Sync1;
      }
      return false;
    case State::Class:
      cls_ = b;
      addCk(b);
      state_ = State::Id;
      return false;
    case State::Id:
      id_ = b;
      addCk(b);
      state_ = State::Len1;
      return false;
    case State::Len1:
      len_ = b;
      addCk(b);
      state_ = State::Len2;
      return false;
    case State::Len2:
      len_ |= static_cast<uint16_t>(b) << 8;
      addCk(b);
      if (len_ > kMaxPayloadLen) {
        lengthErrors_++;
        state_ = State::Sync1;
      } else {
        pos_ = 0;
        state_ = len_ ? State::Payload : State::CkA;
      }
      return false;
    case State::Payload:
      payload_[pos_++] = b;
      addCk(b);
      if (pos_ >= len_) state_ = State::CkA;
      return false;
    case State::CkA:
      rxCkA_ = b;
      state_ = State::CkB;
      return false;
    case State::CkB:
      state_ = State::Sync1;
      if (rxCkA_ == ckA_ && b == ckB_) return true;
      checksumErrors_++;
      return false;
  }
  return false;
}

static uint16_t u16(const uint8_t* p) { return p[0] | (p[1] << 8); }
static int16_t i16(const uint8_t* p) { return static_cast<int16_t>(u16(p)); }
static uint32_t u32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
static int32_t i32(const uint8_t* p) { return static_cast<int32_t>(u32(p)); }

bool decodeData(const uint8_t* p, size_t len, Data& d) {
  if (len < kDataPayloadLen) return false;
  d.iTOW = u32(p + 0);
  d.year = u16(p + 4);
  d.month = p[6];
  d.day = p[7];
  d.hour = p[8];
  d.minute = p[9];
  d.second = p[10];
  d.validityFlags = p[11];
  d.timeAccuracy = u32(p + 12);
  d.nanoseconds = i32(p + 16);
  d.fixStatus = p[20];
  d.fixStatusFlags = p[21];
  d.dateTimeFlags = p[22];
  d.numSV = p[23];
  d.lon = i32(p + 24);
  d.lat = i32(p + 28);
  d.wgsAlt = i32(p + 32);
  d.mslAlt = i32(p + 36);
  d.hAcc = u32(p + 40);
  d.vAcc = u32(p + 44);
  d.speed = i32(p + 48);
  d.heading = i32(p + 52);
  d.speedAcc = u32(p + 56);
  d.headingAcc = u32(p + 60);
  d.pdop = u16(p + 64);
  d.latLonFlags = p[66];
  d.battery = p[67];
  d.gX = i16(p + 68);
  d.gY = i16(p + 70);
  d.gZ = i16(p + 72);
  d.rotX = i16(p + 74);
  d.rotY = i16(p + 76);
  d.rotZ = i16(p + 78);
  return true;
}

// Howard Hinnant's days_from_civil / civil_from_days.
static int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int64_t era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = static_cast<unsigned>(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

static void civilFromDays(int64_t z, int& y, unsigned& m, unsigned& d) {
  z += 719468;
  const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = static_cast<unsigned>(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y = static_cast<int>(static_cast<int64_t>(yoe) + era * 400 + (m <= 2));
}

int64_t unixNanos(const Data& d) {
  const int64_t days = daysFromCivil(d.year, d.month, d.day);
  const int64_t secs = days * 86400 + d.hour * 3600 + d.minute * 60 + d.second;
  return secs * 1000000000LL + d.nanoseconds;
}

static void formatTime(int64_t ns, char* out, size_t size) {
  int64_t secs = ns / 1000000000LL;
  int64_t frac = ns % 1000000000LL;
  if (frac < 0) {
    frac += 1000000000LL;
    secs -= 1;
  }
  int64_t days = secs / 86400;
  int64_t sod = secs % 86400;
  if (sod < 0) {
    sod += 86400;
    days -= 1;
  }
  int y;
  unsigned m, d;
  civilFromDays(days, y, m, d);
  snprintf(out, size, "\"%04d-%02u-%02uT%02d:%02d:%02d.%09ldZ\"", y, m, d,
           static_cast<int>(sod / 3600), static_cast<int>(sod / 60 % 60),
           static_cast<int>(sod % 60), static_cast<long>(frac));
}

void jsonEscape(const char* s, char* out, size_t size) {
  size_t o = 0;
  for (; *s && o + 7 < size; s++) {
    const unsigned char c = static_cast<unsigned char>(*s);
    if (c == '"' || c == '\\') {
      out[o++] = '\\';
      out[o++] = c;
    } else if (c < 0x20) {
      o += snprintf(out + o, size - o, "\\u%04x", c);
    } else {
      out[o++] = c;
    }
  }
  out[o] = '\0';
}

size_t toJson(const Data& d, const JsonMeta& meta, char* buf, size_t size) {
  char time[40] = "null";
  if (d.dateTimeValid()) formatTime(unixNanos(d), time, sizeof(time));

  char device[96];
  jsonEscape(meta.device ? meta.device : "", device, sizeof(device));

  char battery[64];
  if (meta.isMicro) {
    snprintf(battery, sizeof(battery), "{\"input_voltage\":%.1f}", d.battery / 10.0);
  } else {
    snprintf(battery, sizeof(battery), "{\"percent\":%u,\"charging\":%s}",
             d.battery & 0x7F, (d.battery & 0x80) ? "true" : "false");
  }

  const char* t = "true";
  const char* f = "false";
  int n = snprintf(
      buf, size,
      "{\"type\":\"racebox\",\"seq\":%lu,\"device\":\"%s\","
      "\"itow\":%lu,\"time\":%s,\"time_valid\":%s,\"time_confirmed\":%s,"
      "\"time_acc_ns\":%lu,"
      "\"fix\":%u,\"fix_ok\":%s,\"sats\":%u,"
      "\"lat\":%.7f,\"lon\":%.7f,\"latlon_valid\":%s,"
      "\"alt_wgs_m\":%.3f,\"alt_msl_m\":%.3f,\"h_acc_m\":%.3f,\"v_acc_m\":%.3f,"
      "\"speed_mps\":%.3f,\"speed_acc_mps\":%.3f,"
      "\"heading_deg\":%.5f,\"heading_acc_deg\":%.5f,\"heading_valid\":%s,"
      "\"pdop\":%.2f,\"battery\":%s,"
      "\"g\":{\"x\":%.3f,\"y\":%.3f,\"z\":%.3f},"
      "\"gyro_dps\":{\"x\":%.2f,\"y\":%.2f,\"z\":%.2f}}",
      static_cast<unsigned long>(meta.seq), device, static_cast<unsigned long>(d.iTOW), time,
      d.dateTimeValid() ? t : f, d.dateTimeConfirmed() ? t : f,
      static_cast<unsigned long>(d.timeAccuracy), d.fixStatus, d.fixOk() ? t : f, d.numSV,
      d.lat / 1e7, d.lon / 1e7, d.latLonValid() ? t : f, d.wgsAlt / 1e3, d.mslAlt / 1e3,
      d.hAcc / 1e3, d.vAcc / 1e3, d.speed / 1e3, d.speedAcc / 1e3, d.heading / 1e5,
      d.headingAcc / 1e5, d.headingValid() ? t : f, d.pdop / 100.0, battery, d.gX / 1e3,
      d.gY / 1e3, d.gZ / 1e3, d.rotX / 100.0, d.rotY / 100.0, d.rotZ / 100.0);
  if (n < 0 || static_cast<size_t>(n) >= size) return 0;
  return static_cast<size_t>(n);
}

}  // namespace racebox
