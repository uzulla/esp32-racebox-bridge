#include "sdlog.h"

#ifndef RACEBOX_SDLOG

namespace sdlog {
void init() {}
void write(const racebox::Data&, bool) {}
void requestStop() {}
void requestStart() {}
State state() { return State::Off; }
}  // namespace sdlog

#else

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "esp_rom_gpio.h"
#include "esp_timer.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "nvs.h"
#include "sdmmc_cmd.h"
#include "soc/spi_periph.h"

namespace sdlog {
namespace {

const char* TAG = "sdlog";

// Waveshare ESP32-C6-GEEK (v1): the TF slot is on GPIO18-20/23 and the LCD on GPIO1/2/5, but the
// ESP32-C6 has only one general purpose SPI host (SPI2). So SPI2 is initialised once with the TF
// pins, and its clock and MOSI are also routed to the LCD's pins through the GPIO matrix. Each
// device only listens while its own CS is low, so they share the bus without noticing each other.
constexpr spi_host_device_t kHost = SPI2_HOST;
constexpr gpio_num_t kSdClk = GPIO_NUM_19, kSdMosi = GPIO_NUM_18, kSdMiso = GPIO_NUM_20, kSdCs = GPIO_NUM_23;
constexpr gpio_num_t kLcdClk = GPIO_NUM_1, kLcdMosi = GPIO_NUM_2;
constexpr int kBusMaxTransfer = 240 * 135 * 2;  // one LCD frame

constexpr const char* kMount = "/sdcard";
constexpr int kQueueLen = 64;  // about 2.5 s of 25 Hz data
constexpr int64_t kFlushUs = 1000000;

struct Item {
  racebox::Data d;
  int64_t atUs;
  bool micro;
};

std::atomic<State> gState{State::Off};
std::atomic<bool> gStop{false};
QueueHandle_t gQueue;
sdmmc_card_t* gCard;
FILE* gFile;
char gPath[40];
uint32_t gSession;
uint32_t gSeq;
std::atomic<bool> gStarting{false};
uint32_t gDropped;

void routeToLcdPins() {
  const auto& sig = spi_periph_signal[kHost];
  const struct { gpio_num_t pin; uint32_t signal; } map[] = {{kLcdClk, sig.spiclk_out}, {kLcdMosi, sig.spid_out}};
  for (const auto& m : map) {
    esp_rom_gpio_pad_select_gpio(m.pin);
    gpio_set_direction(m.pin, GPIO_MODE_OUTPUT);
    esp_rom_gpio_connect_out_signal(m.pin, m.signal, false, false);
  }
}

bool initBus() {
  spi_bus_config_t bus = {};
  bus.mosi_io_num = kSdMosi;
  bus.miso_io_num = kSdMiso;
  bus.sclk_io_num = kSdClk;
  bus.quadwp_io_num = -1;
  bus.quadhd_io_num = -1;
  bus.max_transfer_sz = kBusMaxTransfer;
  if (spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO) != ESP_OK) return false;
  routeToLcdPins();
  return true;
}

bool mountCard() {
  esp_vfs_fat_mount_config_t mc = {};
  mc.format_if_mount_failed = false;  // never touch a card we cannot read
  mc.max_files = 2;
  mc.allocation_unit_size = 0;
  sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
  slot.gpio_cs = kSdCs;
  slot.host_id = kHost;
  sdmmc_host_t host = SDSPI_HOST_DEFAULT();
  host.slot = kHost;
  const esp_err_t err = esp_vfs_fat_sdspi_mount(kMount, &host, &slot, &mc, &gCard);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "no usable FAT32 card (%s)", esp_err_to_name(err));
    return false;
  }
  ESP_LOGI(TAG, "card %s, %llu MB", gCard->cid.name,
           static_cast<unsigned long long>(gCard->csd.capacity) * gCard->csd.sector_size / (1024 * 1024));
  return true;
}

void unmountCard() {
  esp_vfs_fat_sdcard_unmount(kMount, gCard);
  gCard = nullptr;
}

// Like gmeter: the session number counts up per boot (kept in NVS) and skips numbers already on the card.
uint32_t pickSession() {
  uint32_t sess = 0;
  nvs_handle_t h;
  if (nvs_open("log", NVS_READWRITE, &h) == ESP_OK) {
    nvs_get_u32(h, "session", &sess);
    char path[48];
    for (int tries = 0; tries < 1000; tries++) {
      sess = sess % 9999 + 1;
      snprintf(path, sizeof(path), "%s/log_%04u_01.csv", kMount, static_cast<unsigned>(sess));
      struct stat st;
      if (stat(path, &st) != 0) break;
    }
    nvs_set_u32(h, "session", sess);
    nvs_commit(h);
    nvs_close(h);
  }
  return sess ? sess : 1;
}

bool openLog() {
  // Like gmeter: the file name is log_<session>_<seq>; seq counts up each time recording is restarted.
  struct stat st;
  do {
    gSeq++;
    snprintf(gPath, sizeof(gPath), "%s/log_%04u_%02u.csv", kMount, static_cast<unsigned>(gSession),
             static_cast<unsigned>(gSeq));
  } while (stat(gPath, &st) == 0 && gSeq < 99);
  gFile = fopen(gPath, "w");
  if (!gFile) return false;
  static char buf[4096];
  setvbuf(gFile, buf, _IOFBF, sizeof(buf));
  const int n = fprintf(gFile,
      "# session %u seq %u\n"
      "# RaceBox log (ESP32 bridge). One row per RaceBox data message, t_ms = ms since this file was opened\n"
      "# lat/lon: deg, alt: m (MSL), speed: km/h, heading: deg, hacc: m, acc: G (RaceBox axes), gyro: deg/s, "
      "batt: %% (RaceBox Mini) or V (RaceBox Micro)\n"
      "t_ms,utc,fix,sats,lat,lon,alt_m,speed_kmh,heading_deg,hacc_m,acc_x,acc_y,acc_z,gyro_x,gyro_y,gyro_z,batt,charging\n",
      static_cast<unsigned>(gSession), static_cast<unsigned>(gSeq));
  return n > 0;
}

// Returns false on a write error (disk full, card removed, ...).
bool writeRow(const Item& it, int64_t startUs) {
  const racebox::Data& d = it.d;
  char utc[32] = "";
  if (d.dateTimeValid()) {
    snprintf(utc, sizeof(utc), "%04u-%02u-%02uT%02u:%02u:%02u.%03dZ", d.year, d.month, d.day, d.hour, d.minute,
             d.second, static_cast<int>((d.nanoseconds / 1000000 + 1000) % 1000));
  }
  char batt[12];
  if (it.micro) {
    snprintf(batt, sizeof(batt), "%.1f", (d.battery) / 10.0);
  } else {
    snprintf(batt, sizeof(batt), "%u", d.battery & 0x7F);
  }
  const int n = fprintf(gFile, "%ld,%s,%u,%u,%.7f,%.7f,%.1f,%.2f,%.2f,%.1f,%.3f,%.3f,%.3f,%.2f,%.2f,%.2f,%s,%d\n",
                        static_cast<long>((it.atUs - startUs) / 1000), utc, d.fixStatus, d.numSV, d.lat / 1e7,
                        d.lon / 1e7, d.mslAlt / 1000.0, d.speed * 0.0036, d.heading / 1e5, d.hAcc / 1000.0,
                        d.gX / 1000.0, d.gY / 1000.0, d.gZ / 1000.0, d.rotX / 100.0, d.rotY / 100.0, d.rotZ / 100.0,
                        batt, (!it.micro && (d.battery & 0x80)) ? 1 : 0);
  return n > 0;
}

bool flushFile() { return fflush(gFile) == 0 && fsync(fileno(gFile)) == 0; }

void closeAndUnmount() {
  if (gFile) {
    fclose(gFile);  // errors here were already counted by the last flush
    gFile = nullptr;
  }
  unmountCard();
}

void logTask(void*) {
  const int64_t startUs = esp_timer_get_time();
  int64_t flushedAt = startUs;
  bool failed = false;
  Item it;
  while (!gStop) {
    if (xQueueReceive(gQueue, &it, pdMS_TO_TICKS(200)) == pdTRUE) {
      if (!writeRow(it, startUs)) {
        ESP_LOGE(TAG, "write failed (errno %d), recording stopped", errno);
        failed = true;
        break;
      }
    }
    const int64_t now = esp_timer_get_time();
    if (now - flushedAt >= kFlushUs) {
      flushedAt = now;
      if (!flushFile()) {
        ESP_LOGE(TAG, "flush failed (errno %d), recording stopped", errno);
        failed = true;
        break;
      }
    }
  }
  if (failed) {
    gState = State::Failed;
  } else {
    while (xQueueReceive(gQueue, &it, 0) == pdTRUE) {  // what was queued before the stop
      if (!writeRow(it, startUs)) break;
    }
    flushFile();
  }
  closeAndUnmount();
  if (!failed) gState = State::Safe;
  ESP_LOGI(TAG, "%s %s (%lu dropped)", failed ? "failed:" : "saved", gPath, static_cast<unsigned long>(gDropped));
  vTaskDelete(nullptr);
}

// Creates the next log file on the mounted card and starts the writer task.
bool startLogging() {
  if (!openLog()) {
    ESP_LOGE(TAG, "cannot create %s", gPath);
    closeAndUnmount();
    gState = State::Failed;
    return false;
  }
  gStop = false;
  Item stale;
  while (xQueueReceive(gQueue, &stale, 0) == pdTRUE) {}
  gState = State::Rec;
  xTaskCreate(logTask, "sdlog", 6144, nullptr, 4, nullptr);
  ESP_LOGI(TAG, "recording to %s", gPath);
  return true;
}

// Mounting takes a while, so restarting runs in its own task (the caller is the LCD task).
void restartTask(void*) {
  if (mountCard()) {
    if (!gSession) gSession = pickSession();
    startLogging();
  } else {
    gState = State::Failed;
  }
  gStarting = false;
  vTaskDelete(nullptr);
}

}  // namespace

void init() {
  if (!initBus()) return;
  gQueue = xQueueCreate(kQueueLen, sizeof(Item));
  if (!mountCard()) return;  // no card now: a long press tries again later
  gSession = pickSession();
  startLogging();
}

void write(const racebox::Data& d, bool micro) {
  if (gState != State::Rec) return;
  const Item it{d, esp_timer_get_time(), micro};
  if (xQueueSend(gQueue, &it, 0) != pdTRUE) gDropped++;
}

void requestStop() {
  State rec = State::Rec;
  if (gState.compare_exchange_strong(rec, State::Saving)) gStop = true;
}

void requestStart() {
  const State st = gState;
  if (st == State::Rec || st == State::Saving || !gQueue) return;
  bool idle = false;
  if (gStarting.compare_exchange_strong(idle, true)) {
    xTaskCreate(restartTask, "sdlog_start", 4096, nullptr, 4, nullptr);
  }
}

State state() { return gState; }

}  // namespace sdlog

#endif
