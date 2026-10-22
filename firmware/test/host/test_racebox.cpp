#include <racebox.h>
#include <stdio.h>
#include <string.h>
#include <unity.h>

#include <string>

using namespace racebox;

// Example packet from the RaceBox BLE Protocol Documentation, Revision 9.
static const uint8_t kExample[] = {
    0xB5, 0x62, 0xFF, 0x01, 0x50, 0x00, 0xA0, 0xE7, 0x0C, 0x07, 0xE6, 0x07, 0x01, 0x0A, 0x08,
    0x33, 0x08, 0x37, 0x19, 0x00, 0x00, 0x00, 0x2A, 0xAD, 0x4D, 0x0E, 0x03, 0x01, 0xEA, 0x0B,
    0xC6, 0x93, 0xE1, 0x0D, 0x3B, 0x37, 0x6F, 0x19, 0x61, 0x8C, 0x09, 0x00, 0x0F, 0x01, 0x09,
    0x00, 0x9C, 0x03, 0x00, 0x00, 0x2C, 0x07, 0x00, 0x00, 0x23, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0xD0, 0x00, 0x00, 0x00, 0x88, 0xA9, 0xDD, 0x00, 0x2C, 0x01, 0x00, 0x59, 0xFD,
    0xFF, 0x71, 0x00, 0xCE, 0x03, 0x2F, 0xFF, 0x56, 0x00, 0xFC, 0xFF, 0x06, 0xDB};

static int feedAll(UbxParser& p, const uint8_t* data, size_t len) {
  int packets = 0;
  for (size_t i = 0; i < len; i++) {
    if (p.feed(data[i])) packets++;
  }
  return packets;
}

void setUp() {}
void tearDown() {}

void test_parse_example() {
  UbxParser p;
  TEST_ASSERT_EQUAL(1, feedAll(p, kExample, sizeof(kExample)));
  TEST_ASSERT_EQUAL_HEX8(kDataClass, p.msgClass());
  TEST_ASSERT_EQUAL_HEX8(kDataId, p.msgId());
  TEST_ASSERT_EQUAL(80, p.payloadLen());

  Data d;
  TEST_ASSERT_TRUE(decodeData(p.payload(), p.payloadLen(), d));
  TEST_ASSERT_EQUAL_UINT32(118286240, d.iTOW);
  TEST_ASSERT_EQUAL(2022, d.year);
  TEST_ASSERT_EQUAL(1, d.month);
  TEST_ASSERT_EQUAL(10, d.day);
  TEST_ASSERT_EQUAL(8, d.hour);
  TEST_ASSERT_EQUAL(51, d.minute);
  TEST_ASSERT_EQUAL(8, d.second);
  TEST_ASSERT_TRUE(d.dateTimeValid());
  TEST_ASSERT_TRUE(d.dateTimeConfirmed());
  TEST_ASSERT_EQUAL(239971626, d.nanoseconds);
  TEST_ASSERT_TRUE(d.fixOk());
  TEST_ASSERT_EQUAL(11, d.numSV);
  TEST_ASSERT_EQUAL_INT32(232887238, d.lon);
  TEST_ASSERT_EQUAL_INT32(426719035, d.lat);
  TEST_ASSERT_EQUAL_INT32(625761, d.wgsAlt);
  TEST_ASSERT_EQUAL_INT32(590095, d.mslAlt);
  TEST_ASSERT_EQUAL_UINT32(924, d.hAcc);
  TEST_ASSERT_EQUAL_INT32(35, d.speed);
  TEST_ASSERT_EQUAL_UINT32(14526856, d.headingAcc);
  TEST_ASSERT_EQUAL(300, d.pdop);
  TEST_ASSERT_TRUE(d.latLonValid());
  TEST_ASSERT_EQUAL_HEX8(0x59, d.battery);
  TEST_ASSERT_EQUAL(-3, d.gX);
  TEST_ASSERT_EQUAL(113, d.gY);
  TEST_ASSERT_EQUAL(974, d.gZ);
  TEST_ASSERT_EQUAL(-209, d.rotX);
  TEST_ASSERT_EQUAL(86, d.rotY);
  TEST_ASSERT_EQUAL(-4, d.rotZ);
}

void test_split_and_concatenated_with_garbage() {
  // garbage, packet, packet; fed in odd-sized chunks like BLE notifications.
  std::string s = std::string("\x00\xB5\x11\xB5", 4);
  s.append(reinterpret_cast<const char*>(kExample), sizeof(kExample));
  s.append(reinterpret_cast<const char*>(kExample), sizeof(kExample));
  UbxParser p;
  int packets = 0;
  for (size_t off = 0; off < s.size(); off += 20) {
    size_t n = s.size() - off < 20 ? s.size() - off : 20;
    packets += feedAll(p, reinterpret_cast<const uint8_t*>(s.data()) + off, n);
  }
  TEST_ASSERT_EQUAL(2, packets);
  TEST_ASSERT_EQUAL(0, p.checksumErrors());
}

void test_bad_checksum_then_recover() {
  uint8_t bad[sizeof(kExample)];
  memcpy(bad, kExample, sizeof(bad));
  bad[30] ^= 0x01;
  UbxParser p;
  TEST_ASSERT_EQUAL(0, feedAll(p, bad, sizeof(bad)));
  TEST_ASSERT_EQUAL(1, p.checksumErrors());
  TEST_ASSERT_EQUAL(1, feedAll(p, kExample, sizeof(kExample)));
}

void test_json() {
  UbxParser p;
  feedAll(p, kExample, sizeof(kExample));
  Data d;
  decodeData(p.payload(), p.payloadLen(), d);
  char buf[1024];
  JsonMeta meta{"RaceBox Mini 0123456789", false, 7};
  size_t n = toJson(d, meta, buf, sizeof(buf));
  TEST_ASSERT_GREATER_THAN(0, n);
  std::string j(buf, n);
  TEST_ASSERT_TRUE(j.find("\"time\":\"2022-01-10T08:51:08.239971626Z\"") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"lat\":42.6719035,\"lon\":23.2887238") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"battery\":{\"percent\":89,\"charging\":false}") != std::string::npos);
  TEST_ASSERT_TRUE(j.find("\"seq\":7,") != std::string::npos);
  TEST_ASSERT_EQUAL(0, toJson(d, meta, buf, 64));
}

void test_negative_nanoseconds_borrow() {
  Data d{};
  d.year = 2024;
  d.month = 1;
  d.day = 1;
  d.validityFlags = 0x07;
  d.nanoseconds = -1000;  // 2023-12-31T23:59:59.999999000Z
  char buf[1024];
  JsonMeta meta{"x", true, 0};
  size_t n = toJson(d, meta, buf, sizeof(buf));
  std::string j(buf, n);
  TEST_ASSERT_TRUE(j.find("\"time\":\"2023-12-31T23:59:59.999999000Z\"") != std::string::npos);

  d.validityFlags = 0x03;  // not fully resolved
  n = toJson(d, meta, buf, sizeof(buf));
  j.assign(buf, n);
  TEST_ASSERT_TRUE(j.find("\"time\":null") != std::string::npos);
}

// Every field at its widest textual form must still fit in one unfragmented
// UDP datagram.
void test_worst_case_json_fits_one_datagram() {
  Data d;
  memset(&d, 0, sizeof(d));
  d.iTOW = UINT32_MAX;
  d.year = 65535;
  d.month = 12;
  d.day = 31;
  d.hour = d.minute = d.second = 59;
  d.validityFlags = 0x07;
  d.timeAccuracy = UINT32_MAX;
  d.nanoseconds = INT32_MIN;
  d.fixStatus = 255;
  d.numSV = 255;
  d.lon = d.lat = INT32_MIN;
  d.wgsAlt = d.mslAlt = INT32_MIN;
  d.hAcc = d.vAcc = d.speedAcc = d.headingAcc = UINT32_MAX;
  d.speed = d.heading = INT32_MIN;
  d.pdop = UINT16_MAX;
  d.battery = 0xFF;
  d.gX = d.gY = d.gZ = d.rotX = d.rotY = d.rotZ = INT16_MIN;

  // Longest name the firmware keeps (63 chars), all needing escapes.
  std::string name(63, '"');
  char buf[4096];
  for (bool micro : {false, true}) {
    JsonMeta meta{name.c_str(), micro, UINT32_MAX};
    size_t n = toJson(d, meta, buf, sizeof(buf));
    TEST_ASSERT_GREATER_THAN(0, n);
    printf("worst-case JSON (%s): %zu bytes\n", micro ? "micro" : "mini", n);
    TEST_ASSERT_LESS_OR_EQUAL(kMaxUdpPayload, n);
  }
}

void test_json_escape() {
  char out[64];
  jsonEscape("a\"b\\c\x01", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\c\\u0001", out);
  jsonEscape("abcdefghij", out, 8);  // truncated, still terminated
  TEST_ASSERT_LESS_THAN(8, strlen(out));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parse_example);
  RUN_TEST(test_split_and_concatenated_with_garbage);
  RUN_TEST(test_bad_checksum_then_recover);
  RUN_TEST(test_json);
  RUN_TEST(test_negative_nanoseconds_borrow);
  RUN_TEST(test_worst_case_json_fits_one_datagram);
  RUN_TEST(test_json_escape);
  return UNITY_END();
}
