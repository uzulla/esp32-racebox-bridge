// RaceBox BLE protocol (Revision 9) parsing, independent of Arduino so it can be
// unit-tested on the host.
#pragma once

#include <stddef.h>
#include <stdint.h>

namespace racebox {

constexpr uint8_t kSync1 = 0xB5;
constexpr uint8_t kSync2 = 0x62;
constexpr uint8_t kDataClass = 0xFF;
constexpr uint8_t kDataId = 0x01;
constexpr size_t kDataPayloadLen = 80;
// The device limits a full packet to 512 bytes (6 header + 504 payload + 2 checksum).
constexpr size_t kMaxPayloadLen = 504;
// Upper bound for one UDP datagram. One unfragmented IPv4 packet on a 1500-byte
// MTU link carries up to 1472 bytes; 1000 leaves headroom for links with a
// smaller MTU (VPN, PPPoE, tunnels).
constexpr size_t kMaxUdpPayload = 1000;

// Reassembles UBX packets from a byte stream. BLE notifications may contain a
// partial packet or several packets, so bytes are fed one at a time.
class UbxParser {
 public:
  // Returns true when a complete packet with a valid checksum has just been
  // received; it can then be read via msgClass()/msgId()/payload()/payloadLen()
  // until the next call to feed().
  bool feed(uint8_t b);
  void reset() { state_ = State::Sync1; }

  uint8_t msgClass() const { return cls_; }
  uint8_t msgId() const { return id_; }
  const uint8_t* payload() const { return payload_; }
  size_t payloadLen() const { return len_; }

  uint32_t checksumErrors() const { return checksumErrors_; }
  uint32_t lengthErrors() const { return lengthErrors_; }

 private:
  enum class State { Sync1, Sync2, Class, Id, Len1, Len2, Payload, CkA, CkB };

  void addCk(uint8_t b) {
    ckA_ += b;
    ckB_ += ckA_;
  }

  State state_ = State::Sync1;
  uint8_t cls_ = 0, id_ = 0;
  uint16_t len_ = 0, pos_ = 0;
  uint8_t ckA_ = 0, ckB_ = 0, rxCkA_ = 0;
  uint8_t payload_[kMaxPayloadLen];
  uint32_t checksumErrors_ = 0;
  uint32_t lengthErrors_ = 0;
};

// Decoded RaceBox Data Message (class 0xFF, id 0x01). Raw integer values are
// kept as in the protocol; conversion to SI units happens in toJson().
struct Data {
  uint32_t iTOW;           // ms since GPS week start
  uint16_t year;
  uint8_t month, day, hour, minute, second;
  uint8_t validityFlags;   // bit0 date valid, bit1 time valid, bit2 fully resolved
  uint32_t timeAccuracy;   // ns
  int32_t nanoseconds;     // signed, may be negative
  uint8_t fixStatus;       // 0 none, 2 2D, 3 3D
  uint8_t fixStatusFlags;  // bit0 valid fix, bit5 valid heading
  uint8_t dateTimeFlags;   // bit5 confirmation available, bit6 date confirmed, bit7 time confirmed
  uint8_t numSV;
  int32_t lon, lat;        // deg * 1e7
  int32_t wgsAlt, mslAlt;  // mm
  uint32_t hAcc, vAcc;     // mm
  int32_t speed;           // mm/s
  int32_t heading;         // deg * 1e5
  uint32_t speedAcc;       // mm/s
  uint32_t headingAcc;     // deg * 1e5
  uint16_t pdop;           // * 100
  uint8_t latLonFlags;     // bit0 invalid lat/lon/alt
  uint8_t battery;         // Mini: bit7 charging, bits6..0 percent. Micro: input voltage * 10
  int16_t gX, gY, gZ;      // milli-g
  int16_t rotX, rotY, rotZ;  // centi-deg/s

  bool dateTimeValid() const { return (validityFlags & 0x07) == 0x07; }
  bool dateTimeConfirmed() const { return (dateTimeFlags & 0xE0) == 0xE0; }
  bool fixOk() const { return fixStatus == 3 && (fixStatusFlags & 0x01); }
  bool latLonValid() const { return !(latLonFlags & 0x01); }
  bool headingValid() const { return fixStatusFlags & 0x20; }
};

bool decodeData(const uint8_t* payload, size_t len, Data& out);

// UTC time of the message as nanoseconds since the Unix epoch, with the
// (possibly negative) nanoseconds field applied.
int64_t unixNanos(const Data& d);

struct JsonMeta {
  const char* device;  // advertised name, e.g. "RaceBox Mini 1234567890"
  bool isMicro;        // battery field holds input voltage instead of percent
  uint32_t seq;
};

// Copies s into out (NUL-terminated, truncated to fit) escaping it for use
// inside a JSON string. Each input byte takes at most 6 output bytes.
void jsonEscape(const char* s, char* out, size_t size);

// Writes one JSON object (no trailing newline). Returns the length written, or
// 0 if the buffer was too small.
size_t toJson(const Data& d, const JsonMeta& meta, char* buf, size_t size);

}  // namespace racebox
