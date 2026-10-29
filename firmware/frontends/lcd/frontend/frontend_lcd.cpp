// Frontend for the Waveshare ESP32-C6-GEEK: status display on its ST7789 LCD (240x135).
// Built only for the c6-geek variant (RACEBOX_FRONTEND=lcd). Interface: bridge_status.h.
//
// The whole frame is drawn into one RGB565 framebuffer and sent over SPI by DMA; the next frame
// is drawn only after the previous transfer has finished. The panel has no TE pin, so there is no
// vsync; at ~25 fps that is not noticeable for a gauge.

#include "bridge_status.h"

#include <math.h>
#include <time.h>
#include <algorithm>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

namespace bridge {
namespace {

const char* TAG = "frontend_lcd";

// Pins and panel geometry, checked on the board (see the c6_geek_lcd project).
constexpr gpio_num_t kSclk = GPIO_NUM_1, kMosi = GPIO_NUM_2, kDc = GPIO_NUM_3, kRst = GPIO_NUM_4,
                     kCs = GPIO_NUM_5, kBl = GPIO_NUM_6, kBtn = GPIO_NUM_9;  // BOOT, low = pressed
constexpr int W = 240, H = 135, kGapX = 40, kGapY = 53;
constexpr int kFrameMs = 40;
constexpr int kPages = 2;

// ST7789 takes big-endian RGB565, so the framebuffer holds byte-swapped values.
constexpr uint16_t rgb(int r, int g, int b) {
  const uint16_t v = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
  return static_cast<uint16_t>((v << 8) | (v >> 8));
}

constexpr uint16_t kBg = rgb(5, 8, 16), kBar = rgb(12, 28, 52), kCyan = rgb(0, 205, 255),
                   kWhite = rgb(235, 240, 250), kDim = rgb(105, 118, 140), kTrack = rgb(26, 33, 48),
                   kGreen = rgb(50, 230, 120), kAmber = rgb(255, 176, 0), kRed = rgb(255, 70, 70),
                   kSpeed = rgb(255, 40, 40), kGray = rgb(150, 156, 168);

#include "font5x7.inc"

uint16_t fb[W * H];
SemaphoreHandle_t txDone;
esp_lcd_panel_handle_t panel;

// ---------------------------------------------------------------------------
// Drawing

inline void px(int x, int y, uint16_t c) {
  if (x >= 0 && x < W && y >= 0 && y < H) fb[y * W + x] = c;
}

void fillRect(int x, int y, int w, int h, uint16_t c) {
  const int x0 = x < 0 ? 0 : x, x1 = x + w > W ? W : x + w;
  const int y0 = y < 0 ? 0 : y, y1 = y + h > H ? H : y + h;
  for (int j = y0; j < y1; j++)
    for (int i = x0; i < x1; i++) fb[j * W + i] = c;
}

void rectOutline(int x, int y, int w, int h, uint16_t c) {
  fillRect(x, y, w, 1, c);
  fillRect(x, y + h - 1, w, 1, c);
  fillRect(x, y, 1, h, c);
  fillRect(x + w - 1, y, 1, h, c);
}

void disc(int cx, int cy, int r, uint16_t c) {
  for (int j = -r; j <= r; j++)
    for (int i = -r; i <= r; i++)
      if (i * i + j * j <= r * r + r) px(cx + i, cy + j, c);
}

constexpr int kGlyphW = 6;  // 5 columns + 1 gap

// Draws s with the 5x7 font enlarged `scale` times. Returns the x after the last character.
int text(int x, int y, const char* s, int scale, uint16_t c) {
  for (; *s; s++, x += kGlyphW * scale) {
    const char ch = (*s < 0x20 || *s > 0x7E) ? '?' : *s;
    const uint8_t* g = FONT5X7[ch - 0x20];
    for (int cx = 0; cx < 5; cx++)
      for (int cy = 0; cy < 7; cy++)
        if (g[cx] & (1 << cy)) fillRect(x + cx * scale, y + cy * scale, scale, scale, c);
  }
  return x;
}

int textWidth(const char* s, int scale) { return static_cast<int>(strlen(s)) * kGlyphW * scale; }

int textRight(int xRight, int y, const char* s, int scale, uint16_t c) {
  return text(xRight - textWidth(s, scale) + scale, y, s, scale, c);  // the last gap column is blank
}

// ---------------------------------------------------------------------------
// State

Status st;
portMUX_TYPE stLock = portMUX_INITIALIZER_UNLOCKED;
int page = 0;

// Speed in km/h * 10.
int kmh10(int32_t mms) { return static_cast<int>(static_cast<int64_t>(mms) * 36 / 1000); }

// Dynamic G: the length of the acceleration vector minus the gravity vector measured while
// standing still, so it reads 0.00 at rest. The mounting direction is unknown, so longitudinal and
// lateral G are not separated. Works without GPS: "still" is then judged from the IMU alone (no
// rotation, steady acceleration for ~1 s); with a fix, speed < 1 km/h decides and a moving
// vehicle never updates the reference. -1 until the first calibration.
int dynMg = -1;

void updateDynG(const Status& s) {
  static float ref[3], prev[3];
  static int still = 0;
  static bool haveRef = false;
  static int64_t lastData = 0;
  if (!s.dataAtUs || s.dataAtUs == lastData) return;  // once per RaceBox message
  lastData = s.dataAtUs;
  const float g[3] = {static_cast<float>(s.gX), static_cast<float>(s.gY), static_cast<float>(s.gZ)};
  float jerk = 0;
  for (int i = 0; i < 3; i++) jerk = fmaxf(jerk, fabsf(g[i] - prev[i])), prev[i] = g[i];
  const int rot = std::max({abs(s.rotX), abs(s.rotY), abs(s.rotZ)});  // centi-deg/s
  const bool calm = rot < 500 && (s.fixOk ? kmh10(s.speedMms) < 10 : jerk < 30);
  still = calm ? (still < 1000 ? still + 1 : still) : 0;
  if (still >= 25) {  // about a second at 25 messages/s
    for (int i = 0; i < 3; i++) ref[i] = haveRef ? ref[i] + (g[i] - ref[i]) * 0.05f : g[i];
    haveRef = true;
  }
  if (!haveRef) return;
  const float dx = g[0] - ref[0], dy = g[1] - ref[1], dz = g[2] - ref[2];
  dynMg = static_cast<int>(sqrtf(dx * dx + dy * dy + dz * dz));
}

void fmtCoord(char* out, size_t n, int32_t v) {  // 6 decimals, 1e-7 degree input
  const int64_t a = v < 0 ? -static_cast<int64_t>(v) : v;
  snprintf(out, n, "%s%d.%06d", v < 0 ? "-" : "", static_cast<int>(a / 10000000),
           static_cast<int>(a % 10000000 / 10));
}

void drawSignal(int x, int y, int bars, uint16_t on) {  // 4 bars, 3 px wide, up to 13 px high
  for (int i = 0; i < 4; i++) {
    const int h = 4 + i * 3;
    fillRect(x + i * 4, y + 13 - h, 3, h, i < bars ? on : kTrack);
  }
}

void drawBattery(int x, int y, int pct, bool charging) {
  const uint16_t c = pct < 0 ? kDim : (charging ? kCyan : pct < 20 ? kRed : pct < 50 ? kAmber : kGreen);
  rectOutline(x, y, 19, 11, kDim);
  fillRect(x + 19, y + 3, 2, 5, kDim);
  if (pct >= 0) fillRect(x + 2, y + 2, 15 * pct / 100, 7, c);
}

// Battery in the top-right corner of both pages: icon, then the percentage.
void drawTopBattery(const Status& s, bool fresh) {
  char b[16] = "--";
  const bool known = fresh && s.batteryPct >= 0;
  if (known) snprintf(b, sizeof(b), "%d%%", s.batteryPct);
  const int x = W - 4 - textWidth(b, 1) - 3 - 21;
  drawBattery(x, 3, known ? s.batteryPct : -1, s.charging);
  text(x + 24, 5, b, 1, !known ? kDim : s.charging ? kCyan : kWhite);
}

// SD log state: a dot and a word. Nothing while there is no card.
// While recording the dot blinks at 1 Hz (unless `dot` is false: the page shows its own big one).
void drawRec(int x, int y, int rec, int scale, int64_t now, bool dot = true) {
  static const struct { const char* word; uint16_t col; } kRec[] = {
      {nullptr, 0}, {"REC", kRed}, {"SAVING", kAmber}, {"SAFE", kGreen}, {"SD ERR", kRed}};
  if (rec < 1 || rec > 4) return;
  if (dot && (rec != 1 || (now / 500000) % 2 == 0)) disc(x + 2 + 2 * scale, y + 3 * scale, 1 + 2 * scale, kRec[rec].col);
  text(x + 6 + 4 * scale, y, kRec[rec].word, scale, kRec[rec].col);
}

// Big red dot blinking at 1 Hz while the SD card is being recorded to; visible at a glance.
void drawRecDot(int cx, int cy, int r, int rec, int64_t now) {
  if (rec == 1 && (now / 500000) % 2 == 0) disc(cx, cy, r, kRed);
}

// ---------------------------------------------------------------------------
// Page 1: status

void drawStatusPage(const Status& s, int rate, int64_t now) {
  const bool fresh = s.dataAtUs && now - s.dataAtUs < 2000000;
  char b[40];

  fillRect(0, 0, W, 18, kBar);
  fillRect(0, 18, W, 1, kCyan);
  text(6, 2, "RACEBOX", 2, kCyan);
  if (s.rec) drawRec(94, 2, s.rec, 2, now);
  else text(96, 2, "BRIDGE", 2, kDim);
  drawTopBattery(s, fresh);

  auto label = [](int y, const char* t) { text(22, y, t, 2, kDim); };
  constexpr int kValX = 74;

  // BLE
  int y = 24;
  const char* bleText[] = {"SCANNING", "CONNECTING", "CONNECTED"};
  const uint16_t bleCol = s.ble == 2 ? kGreen : kAmber;
  disc(10, y + 7, 4, bleCol);
  label(y, "BLE");
  text(kValX, y, bleText[s.ble < 3 ? s.ble : 0], 2, bleCol);
  if (s.ble == 2) {
    snprintf(b, sizeof(b), "%dHZ", rate);
    text(200, y, b, 1, kCyan);
  }
  if (s.configWindow) text(200, y + 9, "CFG", 1, kAmber);

  // Wi-Fi
  y += 18;
  const int bars = !s.wifiUp ? 0 : s.rssi > -55 ? 4 : s.rssi > -65 ? 3 : s.rssi > -75 ? 2 : 1;
  drawSignal(2, y + 1, bars, s.wifiUp ? kGreen : kDim);
  if (!s.wifiConfigured) {
    label(y, "WIFI");
    text(kValX, y, "NOT SET", 2, kAmber);
  } else if (!s.wifiUp) {
    label(y, "WIFI");
    text(kValX, y, "JOINING", 2, kAmber);
  } else {
    // No label: the longest IP (15 characters, 180 px) must fit left of the RSSI.
    text(22, y, s.ip, 2, kWhite);
    snprintf(b, sizeof(b), "%d", s.rssi);
    text(W - 24, y, b, 1, kWhite);
    text(W - 24, y + 9, "dBm", 1, kDim);
  }

  // GPS
  y += 18;
  const bool fix = fresh && s.fixOk;
  disc(10, y + 7, 4, fix ? kGreen : fresh ? kAmber : kDim);
  label(y, "GPS");
  if (fresh) {
    snprintf(b, sizeof(b), "%s %dSAT", s.fixOk ? "3D FIX" : s.fix == 2 ? "2D FIX" : "NO FIX", s.sats);
    text(kValX, y, b, 2, fix ? kGreen : kAmber);
  } else {
    text(kValX, y, "--", 2, kDim);
  }

  // Position
  y += 18;
  label(y, "LAT");
  if (fix) {
    fmtCoord(b, sizeof(b), s.lat);
    text(kValX, y, b, 2, kWhite);
    snprintf(b, sizeof(b), "%dM", static_cast<int>(s.altMm / 1000));
    textRight(W - 2, y, "ALT", 1, kDim);
    textRight(W - 2, y + 9, b, 1, kWhite);
  } else {
    text(kValX, y, "--", 2, kDim);
  }
  y += 18;
  label(y, "LON");
  if (fix) {
    fmtCoord(b, sizeof(b), s.lon);
    text(kValX, y, b, 2, kWhite);
    const uint32_t acc = s.hAccMm > 99900 ? 99900 : s.hAccMm;  // keeps it under 6 characters
    snprintf(b, sizeof(b), "%d.%dM", static_cast<int>(acc / 1000), static_cast<int>(acc % 1000 / 100));
    textRight(W - 2, y, "ACC", 1, kDim);
    textRight(W - 2, y + 9, b, 1, kWhite);
  } else {
    text(kValX, y, "--", 2, kDim);
  }

  // Date and time in JST (UTC+9, no DST), from the RaceBox clock; it keeps running between messages.
  y += 18;
  if (s.unixUs) {
    const time_t t = static_cast<time_t>((s.unixUs + (now - s.dataAtUs)) / 1000000) + 9 * 3600;
    tm jst;
    gmtime_r(&t, &jst);
    snprintf(b, sizeof(b), "%04d-%02d-%02d %02d:%02d:%02d", jst.tm_year + 1900, jst.tm_mon + 1, jst.tm_mday,
             jst.tm_hour, jst.tm_min, jst.tm_sec);
    text((W - textWidth(b, 2)) / 2, y, b, 2, fresh ? kWhite : kDim);
  } else {
    text((W - textWidth("----------", 2)) / 2, y, "----------", 2, kDim);
  }
}

// ---------------------------------------------------------------------------
// Page 2: digital speedometer

int lerp(int a, int b, int t, int tMax) { return a + (b - a) * t / tMax; }

void drawSpeedPage(const Status& s, int64_t now) {
  const bool fresh = s.dataAtUs && now - s.dataAtUs < 2000000;
  const bool good = fresh && s.fixOk;
  const int v10 = good ? kmh10(s.speedMms) : 8880;  // 888 = no speed available (no fix / no data)
  char b[16];

  drawTopBattery(s, fresh);

  // G bar in the margin left of the digits, full height of the LCD: 0 at the bottom, kBarMaxMg at the top.
  constexpr int kBarX = 26, kBarW = 20, kBarY = 0, kBarH = H, kBarMaxMg = 1500;
  fillRect(kBarX, kBarY, kBarW, kBarH, kTrack);
  if (fresh && dynMg >= 0) {
    const int lit = (dynMg > kBarMaxMg ? kBarMaxMg : dynMg) * kBarH / kBarMaxMg;
    for (int i = 0; i < lit; i++) {  // row colour follows its height: green -> amber -> red
      const int f = i * 1000 / kBarH;
      const uint16_t c = f < 500 ? rgb(lerp(50, 255, f, 500), lerp(230, 176, f, 500), lerp(120, 0, f, 500))
                                 : rgb(255, lerp(176, 70, f - 500, 500), lerp(0, 70, f - 500, 500));
      fillRect(kBarX, kBarY + kBarH - 1 - i, kBarW, 1, c);
    }
  }
  // Gauge lines every 0.5 G: a dark divider across the bar, and a tick plus label on its left.
  for (int g = 1; g <= 3; g++) {
    const int yl = kBarY + kBarH - kBarH * g / 3;
    if (g < 3) fillRect(kBarX, yl, kBarW, 1, kBg);
    fillRect(kBarX - 5, yl, 4, 1, kDim);
    static const char* const kLabels[] = {"0.5", "1.0", "1.5"};
    text(kBarX - 5 - textWidth(kLabels[g - 1], 1) + 1, g == 3 ? yl + 1 : yl - 3, kLabels[g - 1], 1, kDim);  // top label sits below its line
  }

  // Speed: 3 digits at scale 11 are 198 px wide and 77 px high.
  snprintf(b, sizeof(b), "%d", v10 / 10);
  textRight(W - 4, 24, b, 11, good ? kSpeed : kAmber);

  fillRect(kBarX + kBarW + 4, 108, W - kBarX - kBarW - 4, 1, kTrack);

  // Heading, top center.
  if (fresh && s.headingValid) {
    static const char* const dirs[8] = {"N", "NE", "E", "SE", "S", "SW", "W", "NW"};
    const int deg = (s.headingE5 / 100000 % 360 + 360) % 360;
    snprintf(b, sizeof(b), "%03d %s", deg, dirs[((deg + 22) / 45) % 8]);
  } else {
    snprintf(b, sizeof(b), "---");
  }
  text((W - textWidth(b, 2)) / 2, 4, b, 2, fresh && s.headingValid ? kWhite : kDim);

  drawRec(50, 118, s.rec, 1, now, false);
  drawRecDot(11, 11, 8, s.rec, now);

  // Bottom row: G, unit.
  if (fresh && dynMg >= 0) {
    snprintf(b, sizeof(b), "%d.%02dG", dynMg / 1000, dynMg % 1000 / 10);
    text(96, 115, b, 2, kWhite);
  } else {
    text(96, 115, "---", 2, kDim);
  }
  textRight(W - 4, 115, "KM/H", 2, kGray);
}

// ---------------------------------------------------------------------------
// LCD, button, task

bool onTxDone(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
  BaseType_t woken = pdFALSE;
  xSemaphoreGiveFromISR(txDone, &woken);
  return woken == pdTRUE;
}

void initLcd() {
  gpio_config_t bl = {};
  bl.pin_bit_mask = 1ULL << kBl;
  bl.mode = GPIO_MODE_OUTPUT;
  gpio_config(&bl);
  gpio_set_level(kBl, 0);
  gpio_config_t btn = {};
  btn.pin_bit_mask = 1ULL << kBtn;
  btn.mode = GPIO_MODE_INPUT;
  btn.pull_up_en = GPIO_PULLUP_ENABLE;
  gpio_config(&btn);

  txDone = xSemaphoreCreateBinary();
  xSemaphoreGive(txDone);

#ifndef RACEBOX_SDLOG  // with the SD log, components/sdlog owns the bus (SPI2) and mirrors its pins to ours
  spi_bus_config_t bus = {};
  bus.sclk_io_num = kSclk;
  bus.mosi_io_num = kMosi;
  bus.miso_io_num = -1;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = W * H * 2;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO));
#endif

  esp_lcd_panel_io_handle_t io;
  esp_lcd_panel_io_spi_config_t ioc = {};
  ioc.dc_gpio_num = kDc;
  ioc.cs_gpio_num = kCs;
  ioc.pclk_hz = 40 * 1000 * 1000;
  ioc.lcd_cmd_bits = 8;
  ioc.lcd_param_bits = 8;
  ioc.spi_mode = 0;
  ioc.trans_queue_depth = 2;
  ioc.on_color_trans_done = onTxDone;
  ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(static_cast<esp_lcd_spi_bus_handle_t>(SPI2_HOST), &ioc, &io));

  esp_lcd_panel_dev_config_t pc = {};
  pc.reset_gpio_num = kRst;
  pc.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
  pc.bits_per_pixel = 16;
  ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &pc, &panel));
  ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
  ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(panel, true));
  ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel, true, false));
  ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, kGapX, kGapY));
  for (auto& p : fb) p = kBg;
  ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, fb));
  ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
  gpio_set_level(kBl, 1);
}

// Short press (on release): next page. Long press (while held): stop the SD recording.
void pollButton(int64_t now) {
  static int64_t downAt = 0;
  static bool longDone = false;
  const bool down = gpio_get_level(kBtn) == 0;
  if (down && !downAt) {
    downAt = now;
    longDone = false;
  } else if (down && !longDone && now - downAt >= 1500000) {
    longDone = true;
    onLongPress();
  } else if (!down && downAt) {
    if (!longDone && now - downAt >= 30000) page = (page + 1) % kPages;
    downAt = 0;
  }
}

void displayTask(void*) {
  Status s;
  uint32_t lastPackets = 0;
  int64_t rateAt = esp_timer_get_time();
  int rate = 0;
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    const int64_t now = esp_timer_get_time();
    pollButton(now);
    portENTER_CRITICAL(&stLock);
    s = st;
    portEXIT_CRITICAL(&stLock);
    updateDynG(s);
    if (now - rateAt >= 1000000) {
      rate = static_cast<int>((s.dataPackets - lastPackets) * 1000000LL / (now - rateAt));
      lastPackets = s.dataPackets;
      rateAt = now;
    }

    xSemaphoreTake(txDone, portMAX_DELAY);  // the previous frame is still being sent from fb
    fillRect(0, 0, W, H, kBg);
    if (page == 0) {
      drawStatusPage(s, rate, now);
    } else {
      drawSpeedPage(s, now);
    }
    esp_lcd_panel_draw_bitmap(panel, 0, 0, W, H, fb);
    vTaskDelayUntil(&wake, pdMS_TO_TICKS(kFrameMs));
  }
}

}  // namespace

void frontendStart() {
  initLcd();
  xTaskCreate(displayTask, "display", 6144, nullptr, 3, nullptr);
  ESP_LOGI(TAG, "LCD ready");
}

void frontendUpdate(const Status& s) {
  portENTER_CRITICAL(&stLock);
  st = s;
  portEXIT_CRITICAL(&stLock);
}

}  // namespace bridge
