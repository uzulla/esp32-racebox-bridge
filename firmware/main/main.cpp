// RaceBox Mini (BLE) -> ESP32 -> UDP broadcast (JSON) bridge.

#include <stdio.h>
#include <string.h>

#include <atomic>
#include <string_view>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_pm.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_mac.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "lwip/sockets.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "ap_credentials.h"  // generated per build, see CMakeLists.txt
#include "bridge_status.h"
#include "racebox.h"
#include "sdlog.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char* TAG = "racebox";

// ---------------------------------------------------------------------------
// Shared state

enum class BleState { Scanning, Connecting, Connected };
static std::atomic<BleState> bleState{BleState::Scanning};
// Bumped on every new connection so the data task resets its parser.
static std::atomic<uint32_t> connGeneration{0};
static char targetName[64];

// Notification bytes are handed from the NimBLE host task to the data task here.
static StreamBufferHandle_t rxStream;
static std::atomic<uint32_t> droppedBytes{0};

static int udpSock = -1;
// Subnet broadcast address (network byte order); 0 while Wi-Fi is down.
static std::atomic<uint32_t> broadcastAddr{0};
// Same for the soft-AP subnet, and the number of stations joined to it.
static uint32_t apBroadcastAddr = 0;
static std::atomic<int> apClients{0};
static std::atomic<uint32_t> udpErrors{0};
static bool wifiConfigured = false;
static char wifiSsid[33];  // configured SSID, "" if none

// ---------------------------------------------------------------------------
// UDP

static void sendUdpTo(uint32_t addr, const char* buf, size_t len) {
  sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_port = htons(CONFIG_RACEBOX_UDP_PORT);
  to.sin_addr.s_addr = addr;
  // Never block: a datagram that cannot go out now is stale anyway.
  if (sendto(udpSock, buf, len, MSG_DONTWAIT, reinterpret_cast<sockaddr*>(&to), sizeof(to)) < 0) {
    udpErrors++;
  }
}

// To the joined Wi-Fi network (STA) and, while someone is connected to our own AP, to the AP subnet.
static void sendUdp(const char* buf, size_t len) {
  if (const uint32_t addr = broadcastAddr) sendUdpTo(addr, buf, len);
  if (apClients > 0 && apBroadcastAddr) sendUdpTo(apBroadcastAddr, buf, len);
}

// ---------------------------------------------------------------------------
// Wi-Fi (credentials in NVS namespace "wifi", keys "ssid"/"pass")

// Validates and stores credentials. Returns an error message, or nullptr.
static const char* saveWifiCredentials(const char* ssid, const char* pass) {
  const size_t ssidLen = strlen(ssid);
  const size_t passLen = strlen(pass);
  if (ssidLen == 0 || ssidLen > 32) return "SSID must be 1-32 bytes";
  if (passLen > 64) return "password must be at most 64 bytes";
  nvs_handle_t h;
  if (nvs_open("wifi", NVS_READWRITE, &h) != ESP_OK) return "nvs open failed";
  const bool ok = nvs_set_str(h, "ssid", ssid) == ESP_OK && nvs_set_str(h, "pass", pass) == ESP_OK &&
                  nvs_commit(h) == ESP_OK;
  nvs_close(h);
  return ok ? nullptr : "nvs write failed";
}

static void clearWifiCredentials() {
  nvs_handle_t h;
  if (nvs_open("wifi", NVS_READWRITE, &h) == ESP_OK) {
    nvs_erase_all(h);
    nvs_commit(h);
    nvs_close(h);
  }
}

static void applyWifiConfig() {
  wifi_config_t cfg = {};
  nvs_handle_t h;
  size_t ssidLen = sizeof(cfg.sta.ssid);
  size_t passLen = sizeof(cfg.sta.password);
  wifiConfigured = false;
  if (nvs_open("wifi", NVS_READONLY, &h) == ESP_OK) {
    wifiConfigured =
        nvs_get_str(h, "ssid", reinterpret_cast<char*>(cfg.sta.ssid), &ssidLen) == ESP_OK &&
        cfg.sta.ssid[0];
    if (nvs_get_str(h, "pass", reinterpret_cast<char*>(cfg.sta.password), &passLen) != ESP_OK) {
      cfg.sta.password[0] = '\0';
    }
    nvs_close(h);
  }
  snprintf(wifiSsid, sizeof(wifiSsid), "%s", wifiConfigured ? (const char*)cfg.sta.ssid : "");
  esp_wifi_disconnect();
  if (!wifiConfigured) {
    ESP_LOGW(TAG, "Wi-Fi not configured; run tools/set-wifi.py");
    return;
  }
  ESP_LOGI(TAG, "Wi-Fi connecting to \"%s\"", cfg.sta.ssid);
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
  esp_wifi_connect();
}

static void refreshApClients() {
  wifi_sta_list_t list;
  apClients = esp_wifi_ap_get_sta_list(&list) == ESP_OK ? list.num : 0;
}

static void onWifiEvent(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && (id == WIFI_EVENT_AP_STACONNECTED || id == WIFI_EVENT_AP_STADISCONNECTED)) {
    refreshApClients();
    ESP_LOGI(TAG, "AP: %d station(s) connected", apClients.load());
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    broadcastAddr = 0;
    if (wifiConfigured) esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    const auto* ev = static_cast<ip_event_got_ip_t*>(data);
    const uint32_t bcast = ev->ip_info.ip.addr | ~ev->ip_info.netmask.addr;
    broadcastAddr = bcast;
    ESP_LOGI(TAG, "Wi-Fi connected: " IPSTR ", broadcast " IPSTR ":%d", IP2STR(&ev->ip_info.ip),
             IP2STR(reinterpret_cast<const esp_ip4_addr_t*>(&bcast)), CONFIG_RACEBOX_UDP_PORT);
  }
}

static void initWifi() {
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  esp_netif_t* apNetif = esp_netif_create_default_wifi_ap();
  wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&init));
  // The credentials already live in our own NVS namespace; don't copy them.
  ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
  ESP_ERROR_CHECK(
      esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, onWifiEvent, nullptr));
  ESP_ERROR_CHECK(
      esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, onWifiEvent, nullptr));
  // AP + STA: the STA joins the configured network, the AP lets a phone or PC connect directly
  // (no router at the track). The radio has one channel, so the AP follows the STA's channel.
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  wifi_config_t ap = {};
  strlcpy(reinterpret_cast<char*>(ap.ap.ssid), RACEBOX_AP_SSID, sizeof(ap.ap.ssid));
  strlcpy(reinterpret_cast<char*>(ap.ap.password), RACEBOX_AP_PASSWORD, sizeof(ap.ap.password));
  ap.ap.ssid_len = strlen(RACEBOX_AP_SSID);
  ap.ap.channel = 1;
  ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
  ap.ap.max_connection = 4;
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  esp_netif_ip_info_t apIp = {};
  if (esp_netif_get_ip_info(apNetif, &apIp) == ESP_OK) apBroadcastAddr = apIp.ip.addr | ~apIp.netmask.addr;
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "AP \"%s\" password \"%s\" at " IPSTR, RACEBOX_AP_SSID, RACEBOX_AP_PASSWORD, IP2STR(&apIp.ip));
  applyWifiConfig();

  udpSock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  const int yes = 1;
  setsockopt(udpSock, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
}

// ---------------------------------------------------------------------------
// BLE central (NimBLE)

static uint8_t ownAddrType;
static uint16_t connHandle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t svcStart, svcEnd, txValHandle, cccdHandle;

// 128-bit UUIDs are stored little-endian.
static constexpr ble_uuid128_t uuid128(const uint8_t (&be)[16]) {
  ble_uuid128_t u{};
  u.u.type = BLE_UUID_TYPE_128;
  for (int i = 0; i < 16; i++) u.value[i] = be[15 - i];
  return u;
}
// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E (UART service) / 6E400003-... (TX)
static const ble_uuid128_t kUartService = uuid128(
    {0x6E, 0x40, 0x00, 0x01, 0xB5, 0xA3, 0xF3, 0x93, 0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E});
static const ble_uuid128_t kUartTx = uuid128(
    {0x6E, 0x40, 0x00, 0x03, 0xB5, 0xA3, 0xF3, 0x93, 0xE0, 0xA9, 0xE5, 0x0E, 0x24, 0xDC, 0xCA, 0x9E});

static int onGapEvent(ble_gap_event* ev, void*);

// 7.5-15 ms interval: the device sends up to 25 messages/s, each split into
// several link-layer packets (it has no data length extension). At the 75 ms it
// picks for itself it can't keep up: its send queue fills and adds ~0.6 s of
// latency while it drops messages.
static const uint16_t kConnItvlMin = BLE_GAP_CONN_ITVL_MS(7.5);
static const uint16_t kConnItvlMax = BLE_GAP_CONN_ITVL_MS(15);
// Shortest interval the RaceBox last asked for (it asks for 20-75 ms a few
// seconds after connecting, every 30 s until satisfied, and disconnects after
// three rejections). We accept its request and then settle on this value.
static uint16_t peerItvlMin = kConnItvlMin;

static void startScan() {
  bleState = BleState::Scanning;
  ble_gap_disc_params params = {};
  params.passive = 0;  // active: the name may only be in the scan response
  params.filter_duplicates = 0;
  params.itvl = BLE_GAP_SCAN_ITVL_MS(100);
  params.window = BLE_GAP_SCAN_WIN_MS(50);
  const int rc = ble_gap_disc(ownAddrType, BLE_HS_FOREVER, &params, onGapEvent, nullptr);
  if (rc == 0) {
    ESP_LOGI(TAG, "BLE scanning for RaceBox...");
  } else if (rc != BLE_HS_EALREADY) {
    ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
  }
}

static void disconnect() { ble_gap_terminate(connHandle, BLE_ERR_REM_USER_CONN_TERM); }

static int onSubscribed(uint16_t conn, const ble_gatt_error* err, ble_gatt_attr*, void*) {
  if (err->status != 0) {
    ESP_LOGE(TAG, "Subscribe failed: %d", err->status);
    disconnect();
    return 0;
  }
  connGeneration++;
  bleState = BleState::Connected;
  ble_gap_conn_desc desc;
  if (ble_gap_conn_find(conn, &desc) == 0) {
    ESP_LOGI(TAG, "Subscribed, MTU=%u, interval=%.2f ms, latency=%u, timeout=%u ms",
             ble_att_mtu(conn), desc.conn_itvl * 1.25, desc.conn_latency,
             desc.supervision_timeout * 10);
  }
  return 0;
}

static int onDescriptor(uint16_t conn, const ble_gatt_error* err, uint16_t, const ble_gatt_dsc* dsc,
                        void*) {
  if (err->status == 0) {
    if (!cccdHandle && dsc->uuid.u.type == BLE_UUID_TYPE_16 &&
        dsc->uuid.u16.value == BLE_GATT_DSC_CLT_CFG_UUID16) {
      cccdHandle = dsc->handle;
    }
    return 0;
  }
  static const uint8_t kEnableNotify[] = {0x01, 0x00};
  if (err->status != BLE_HS_EDONE || !cccdHandle ||
      ble_gattc_write_flat(conn, cccdHandle, kEnableNotify, sizeof(kEnableNotify), onSubscribed,
                           nullptr) != 0) {
    ESP_LOGE(TAG, "TX CCCD not found");
    disconnect();
  }
  return 0;
}

static int onCharacteristic(uint16_t conn, const ble_gatt_error* err, const ble_gatt_chr* chr,
                            void*) {
  if (err->status == 0) {
    txValHandle = chr->val_handle;
    return 0;
  }
  if (err->status != BLE_HS_EDONE || !txValHandle ||
      ble_gattc_disc_all_dscs(conn, txValHandle, svcEnd, onDescriptor, nullptr) != 0) {
    ESP_LOGE(TAG, "UART TX characteristic not found");
    disconnect();
  }
  return 0;
}

static int onService(uint16_t conn, const ble_gatt_error* err, const ble_gatt_svc* svc, void*) {
  if (err->status == 0) {
    svcStart = svc->start_handle;
    svcEnd = svc->end_handle;
    return 0;
  }
  if (err->status != BLE_HS_EDONE || !svcStart ||
      ble_gattc_disc_chrs_by_uuid(conn, svcStart, svcEnd, &kUartTx.u, onCharacteristic,
                                  nullptr) != 0) {
    ESP_LOGE(TAG, "UART service not found");
    disconnect();
  }
  return 0;
}

// GATT procedures are run one after another: MTU, service, characteristic,
// descriptor, then the CCCD write that starts the notifications.
static void discoverService(uint16_t conn) {
  const int rc = ble_gattc_disc_svc_by_uuid(conn, &kUartService.u, onService, nullptr);
  if (rc != 0) {
    ESP_LOGE(TAG, "Service discovery failed to start: %d", rc);
    disconnect();
  }
}

static int onMtu(uint16_t conn, const ble_gatt_error* err, uint16_t, void*) {
  if (err->status != 0) ESP_LOGW(TAG, "MTU exchange failed: %d", err->status);
  discoverService(conn);
  return 0;
}

static void onDiscovery(const ble_gap_disc_desc& disc) {
  if (bleState != BleState::Scanning) return;
  ble_hs_adv_fields fields;
  if (ble_hs_adv_parse_fields(&fields, disc.data, disc.length_data) != 0 ||
      !fields.name_len) {
    return;
  }
  char name[sizeof(targetName)];
  const size_t len = fields.name_len < sizeof(name) - 1 ? fields.name_len : sizeof(name) - 1;
  memcpy(name, fields.name, len);
  name[len] = '\0';
  const std::string_view sv(name);
  if (!sv.starts_with("RaceBox ") || sv.find(CONFIG_RACEBOX_NAME_FILTER) == sv.npos) return;

  bleState = BleState::Connecting;
  ble_gap_disc_cancel();
  memcpy(targetName, name, sizeof(name));
  ESP_LOGI(TAG, "Connecting to %s", targetName);

  ble_gap_conn_params params = {};
  params.scan_itvl = BLE_GAP_SCAN_ITVL_MS(16);
  params.scan_window = BLE_GAP_SCAN_WIN_MS(16);
  params.itvl_min = kConnItvlMin;
  params.itvl_max = kConnItvlMax;
  params.latency = 0;
  params.supervision_timeout = BLE_GAP_SUPERVISION_TIMEOUT_MS(4000);
  const int rc = ble_gap_connect(ownAddrType, &disc.addr, 5000, &params, onGapEvent, nullptr);
  if (rc != 0) {
    ESP_LOGE(TAG, "ble_gap_connect failed: %d", rc);
    startScan();
  }
}

static void pushNotification(os_mbuf* om) {
  uint8_t buf[256];
  const uint16_t total = OS_MBUF_PKTLEN(om);
  for (uint16_t off = 0; off < total;) {
    const uint16_t n = total - off < sizeof(buf) ? total - off : sizeof(buf);
    os_mbuf_copydata(om, off, n, buf);
    const size_t sent = xStreamBufferSend(rxStream, buf, n, 0);
    if (sent < n) droppedBytes += n - sent;
    off += n;
  }
}

static int onGapEvent(ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_DISC:
      onDiscovery(ev->disc);
      break;
    case BLE_GAP_EVENT_DISC_COMPLETE:
      if (bleState == BleState::Scanning) startScan();
      break;
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status != 0) {
        ESP_LOGW(TAG, "Connect failed: %d", ev->connect.status);
        startScan();
        break;
      }
      connHandle = ev->connect.conn_handle;
      peerItvlMin = kConnItvlMin;
      svcStart = svcEnd = txValHandle = cccdHandle = 0;
      // The peer may have exchanged the MTU already; go on without it then.
      if (ble_gattc_exchange_mtu(connHandle, onMtu, nullptr) != 0) discoverService(connHandle);
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      ESP_LOGW(TAG, "BLE disconnected, reason=0x%x", ev->disconnect.reason);
      connHandle = BLE_HS_CONN_HANDLE_NONE;
      startScan();
      break;
    case BLE_GAP_EVENT_L2CAP_UPDATE_REQ: {
      // This path can only accept the peer's values as they are or reject them,
      // so accept and narrow the interval afterwards (BLE_GAP_EVENT_CONN_UPDATE).
      const ble_gap_upd_params* peer = ev->conn_update_req.peer_params;
      peerItvlMin = peer->itvl_min > kConnItvlMin ? peer->itvl_min : kConnItvlMin;
      ESP_LOGI(TAG, "RaceBox asked for interval %.2f-%.2f ms", peer->itvl_min * 1.25,
               peer->itvl_max * 1.25);
      break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE_REQ: {
      // Link-layer request: pick the shortest interval within its range.
      ble_gap_upd_params* self = ev->conn_update_req.self_params;
      peerItvlMin = self->itvl_min > kConnItvlMin ? self->itvl_min : kConnItvlMin;
      ESP_LOGI(TAG, "RaceBox asked for interval %.2f-%.2f ms", self->itvl_min * 1.25,
               self->itvl_max * 1.25);
      self->itvl_min = self->itvl_max = peerItvlMin;
      self->latency = 0;
      break;
    }
    case BLE_GAP_EVENT_CONN_UPDATE: {
      ble_gap_conn_desc desc;
      if (ble_gap_conn_find(ev->conn_update.conn_handle, &desc) != 0) break;
      ESP_LOGI(TAG, "Connection updated: status=%d interval=%.2f ms latency=%u",
               ev->conn_update.status, desc.conn_itvl * 1.25, desc.conn_latency);
      if (ev->conn_update.status == 0 && desc.conn_itvl > peerItvlMin) {
        ble_gap_upd_params params = {};
        params.itvl_min = params.itvl_max = peerItvlMin;
        params.latency = 0;
        params.supervision_timeout = desc.supervision_timeout;
        const int rc = ble_gap_update_params(ev->conn_update.conn_handle, &params);
        if (rc != 0) ESP_LOGW(TAG, "ble_gap_update_params failed: %d", rc);
      }
      break;
    }
    case BLE_GAP_EVENT_NOTIFY_RX:
      if (ev->notify_rx.attr_handle == txValHandle) pushNotification(ev->notify_rx.om);
      break;
    default:
      break;
  }
  return 0;
}

// ---------------------------------------------------------------------------
// BLE peripheral: config service. Advertised for CONFIG_RACEBOX_CONFIG_WINDOW_S
// seconds after boot; a client that connects within the window may stay until it
// disconnects. Both characteristics need an encrypted link (Just Works pairing,
// no bonding), so the password is not sent in the clear.
//   status (read):  JSON, see statusJson()
//   wifi   (write): "<ssid>\t<password>", saved to NVS and applied at once

// 9b3c0001-4e5f-4a2b-8c6d-2f1e0a7b5c3d and ...0002 / ...0003
static constexpr ble_uuid128_t kConfigService = uuid128(
    {0x9B, 0x3C, 0x00, 0x01, 0x4E, 0x5F, 0x4A, 0x2B, 0x8C, 0x6D, 0x2F, 0x1E, 0x0A, 0x7B, 0x5C, 0x3D});
static constexpr ble_uuid128_t kConfigStatus = uuid128(
    {0x9B, 0x3C, 0x00, 0x02, 0x4E, 0x5F, 0x4A, 0x2B, 0x8C, 0x6D, 0x2F, 0x1E, 0x0A, 0x7B, 0x5C, 0x3D});
static constexpr ble_uuid128_t kConfigWifi = uuid128(
    {0x9B, 0x3C, 0x00, 0x03, 0x4E, 0x5F, 0x4A, 0x2B, 0x8C, 0x6D, 0x2F, 0x1E, 0x0A, 0x7B, 0x5C, 0x3D});

static std::atomic<bool> configWindowOpen{true};
static std::atomic<uint16_t> configConnHandle{BLE_HS_CONN_HANDLE_NONE};

// {"racebox":"connected","racebox_device":"RaceBox Mini 1254301304",
//  "wifi_ssid":"...","wifi_connected":true,"ip":"192.168.11.38"}
static size_t statusJson(char* buf, size_t size) {
  static const char* const kStates[] = {"scanning", "connecting", "connected"};
  const BleState state = bleState;
  char device[sizeof(targetName) * 6];
  char ssid[sizeof(wifiSsid) * 6];
  racebox::jsonEscape(state == BleState::Scanning ? "" : targetName, device, sizeof(device));
  racebox::jsonEscape(wifiSsid, ssid, sizeof(ssid));
  const bool connected = broadcastAddr != 0;
  char ip[24] = "null";
  esp_netif_ip_info_t info = {};
  if (connected &&
      esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &info) == ESP_OK) {
    snprintf(ip, sizeof(ip), "\"" IPSTR "\"", IP2STR(&info.ip));
  }
  const int n = snprintf(buf, size,
                         "{\"racebox\":\"%s\",\"racebox_device\":\"%s\",\"wifi_ssid\":\"%s\","
                         "\"wifi_connected\":%s,\"ip\":%s}",
                         kStates[static_cast<int>(state)], device, ssid,
                         connected ? "true" : "false", ip);
  return n > 0 && static_cast<size_t>(n) < size ? n : 0;
}

static int onConfigAccess(uint16_t, uint16_t, ble_gatt_access_ctxt* ctxt, void*) {
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    char json[400];
    const size_t n = statusJson(json, sizeof(json));
    return n && os_mbuf_append(ctxt->om, json, n) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
    char buf[32 + 1 + 64 + 1];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf) - 1, &len) != 0) {
      return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    buf[len] = '\0';
    char* tab = strchr(buf, '\t');
    if (!tab) return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    *tab = '\0';
    if (const char* err = saveWifiCredentials(buf, tab + 1)) {
      ESP_LOGW(TAG, "Wi-Fi write over BLE rejected: %s", err);
      return BLE_ATT_ERR_VALUE_NOT_ALLOWED;
    }
    ESP_LOGI(TAG, "Wi-Fi credentials updated over BLE");
    applyWifiConfig();
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

static const ble_gatt_chr_def kConfigChrs[] = {
    {.uuid = &kConfigStatus.u,
     .access_cb = onConfigAccess,
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_READ_ENC},
    {.uuid = &kConfigWifi.u,
     .access_cb = onConfigAccess,
     .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_ENC},
    {},
};

static const ble_gatt_svc_def kConfigSvcs[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kConfigService.u, .characteristics = kConfigChrs},
    {},
};

static int onConfigGapEvent(ble_gap_event* ev, void*);

static void startConfigAdvertising() {
  if (!configWindowOpen || configConnHandle != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
    return;
  }
  ble_hs_adv_fields adv = {};
  adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  adv.uuids128 = &kConfigService;
  adv.num_uuids128 = 1;
  adv.uuids128_is_complete = 1;
  ble_hs_adv_fields rsp = {};
  const char* name = ble_svc_gap_device_name();
  rsp.name = reinterpret_cast<const uint8_t*>(name);
  rsp.name_len = strlen(name);
  rsp.name_is_complete = 1;
  ble_gap_adv_params params = {};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  params.itvl_min = BLE_GAP_ADV_ITVL_MS(200);
  params.itvl_max = BLE_GAP_ADV_ITVL_MS(300);
  int rc = ble_gap_adv_set_fields(&adv);
  if (rc == 0) rc = ble_gap_adv_rsp_set_fields(&rsp);
  if (rc == 0) rc = ble_gap_adv_start(ownAddrType, nullptr, BLE_HS_FOREVER, &params, onConfigGapEvent,
                                      nullptr);
  if (rc != 0) ESP_LOGE(TAG, "Config advertising failed: %d", rc);
}

static void closeConfigWindow(void*) {
  configWindowOpen = false;
  ble_gap_adv_stop();
  ESP_LOGI(TAG, "Config window closed%s",
           configConnHandle != BLE_HS_CONN_HANDLE_NONE ? " (client stays until it disconnects)" : "");
}

static int onConfigGapEvent(ble_gap_event* ev, void*) {
  switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (ev->connect.status == 0) {
        configConnHandle = ev->connect.conn_handle;
        ESP_LOGI(TAG, "Config client connected");
        // Ask the client to pair right away; macOS does not start pairing on its
        // own when a read fails with "insufficient encryption".
        ble_gap_security_initiate(ev->connect.conn_handle);
      } else {
        startConfigAdvertising();
      }
      break;
    case BLE_GAP_EVENT_DISCONNECT:
      configConnHandle = BLE_HS_CONN_HANDLE_NONE;
      ESP_LOGI(TAG, "Config client disconnected, reason=0x%x", ev->disconnect.reason);
      startConfigAdvertising();
      break;
    case BLE_GAP_EVENT_ENC_CHANGE:
      ESP_LOGI(TAG, "Config link encryption: %s", ev->enc_change.status == 0 ? "on" : "failed");
      break;
    default:
      break;
  }
  return 0;
}

static void onBleSync() {
  ESP_ERROR_CHECK(ble_hs_util_ensure_addr(0));
  ESP_ERROR_CHECK(ble_hs_id_infer_auto(0, &ownAddrType));
  uint8_t mac[6];
  esp_read_mac(mac, ESP_MAC_BT);
  char name[32];
  snprintf(name, sizeof(name), "ESP32-RaceBox-Bridge-%02X%02X", mac[4], mac[5]);
  ble_svc_gap_device_name_set(name);
  startScan();
  startConfigAdvertising();
}

static void onBleReset(int reason) { ESP_LOGE(TAG, "NimBLE reset, reason=%d", reason); }

static void bleHostTask(void*) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

static void initBle() {
  ESP_ERROR_CHECK(nimble_port_init());
  ble_hs_cfg.sync_cb = onBleSync;
  ble_hs_cfg.reset_cb = onBleReset;
  // Encrypted link for the config service without stored bonds (Just Works).
  ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
  ble_hs_cfg.sm_bonding = 0;
  ble_hs_cfg.sm_mitm = 0;
  ble_hs_cfg.sm_sc = 1;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  ESP_ERROR_CHECK(ble_gatts_count_cfg(kConfigSvcs));
  ESP_ERROR_CHECK(ble_gatts_add_svcs(kConfigSvcs));
  nimble_port_freertos_init(bleHostTask);
}

// ---------------------------------------------------------------------------
// Data task: reassemble packets and broadcast them as soon as they arrive.

static racebox::UbxParser parser;
static uint32_t dataPackets = 0;
static uint32_t otherPackets = 0;

// Snapshot for the frontend (the display, if this variant has one). Connection fields are refreshed on every
// call; the RaceBox fields only when a data message is passed.
static void publishStatus(const racebox::Data* d) {
  static bridge::Status ds = {};
  static int64_t apAt = 0;
  ds.ble = static_cast<uint8_t>(bleState.load());
  ds.configWindow = configWindowOpen;
  snprintf(ds.device, sizeof(ds.device), "%s", ds.ble == 0 ? "" : targetName);
  ds.wifiConfigured = wifiConfigured;
  ds.wifiUp = broadcastAddr != 0;
  ds.dataPackets = dataPackets;
  ds.udpErrors = udpErrors;
  ds.rec = static_cast<uint8_t>(sdlog::state());
  const int64_t now = esp_timer_get_time();
  if (now - apAt >= 1000000) {  // RSSI and IP change slowly
    apAt = now;
    wifi_ap_record_t ap;
    ds.rssi = ds.wifiUp && esp_wifi_sta_get_ap_info(&ap) == ESP_OK ? ap.rssi : 0;
    esp_netif_ip_info_t ip = {};
    esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
    snprintf(ds.ip, sizeof(ds.ip), IPSTR, IP2STR(&ip.ip));
    snprintf(ds.ssid, sizeof(ds.ssid), "%s", wifiSsid);
  }
  if (d) {
    const bool micro = strncmp(targetName, "RaceBox Micro", 13) == 0;
    ds.dataAtUs = now;
    ds.unixUs = d->dateTimeValid() ? racebox::unixNanos(*d) / 1000 : 0;
    ds.fix = d->fixStatus;
    ds.fixOk = d->fixOk();
    ds.sats = d->numSV;
    ds.lat = d->lat;
    ds.lon = d->lon;
    ds.altMm = d->mslAlt;
    ds.hAccMm = d->hAcc;
    ds.speedMms = d->speed;
    ds.headingE5 = d->heading;
    ds.headingValid = d->headingValid();
    ds.gX = d->gX;
    ds.gY = d->gY;
    ds.gZ = d->gZ;
    ds.rotX = d->rotX;
    ds.rotY = d->rotY;
    ds.rotZ = d->rotZ;
    ds.batteryPct = micro ? -1 : d->battery & 0x7F;
    ds.charging = !micro && (d->battery & 0x80);
  }
  bridge::frontendUpdate(ds);
}

static void handlePacket() {
  static uint32_t seq = 0;
  if (parser.msgClass() != racebox::kDataClass || parser.msgId() != racebox::kDataId) {
    otherPackets++;
    return;
  }
  racebox::Data d;
  if (!racebox::decodeData(parser.payload(), parser.payloadLen(), d)) return;
  dataPackets++;
  sdlog::write(d, strncmp(targetName, "RaceBox Micro", 13) == 0);
  publishStatus(&d);

  // toJson() returns 0 rather than produce a datagram over the size cap.
  static char json[racebox::kMaxUdpPayload + 1];
  const racebox::JsonMeta meta{targetName, strncmp(targetName, "RaceBox Micro", 13) == 0, seq++};
  const size_t n = racebox::toJson(d, meta, json, sizeof(json));
  if (n) sendUdp(json, n);
}

static void sendStatus() {
  static const char* const kStates[] = {"scanning", "connecting", "connected"};
  const BleState state = bleState;
  int rssi = 0;
  wifi_ap_record_t ap;
  if (broadcastAddr && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) rssi = ap.rssi;
  esp_netif_ip_info_t ip = {};
  esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);

  char json[racebox::kMaxUdpPayload + 1];
  const int n = snprintf(
      json, sizeof(json),
      "{\"type\":\"status\",\"uptime_ms\":%lu,\"ble\":\"%s\",\"device\":\"%s\","
      "\"data_packets\":%lu,\"other_packets\":%lu,\"checksum_errors\":%lu,"
      "\"length_errors\":%lu,\"dropped_bytes\":%lu,\"udp_errors\":%lu,"
      "\"wifi_configured\":%s,\"wifi_rssi\":%d,\"ip\":\"" IPSTR "\",\"free_heap\":%lu}",
      (unsigned long)(esp_timer_get_time() / 1000), kStates[static_cast<int>(state)],
      state == BleState::Scanning ? "" : targetName, (unsigned long)dataPackets,
      (unsigned long)otherPackets, (unsigned long)parser.checksumErrors(),
      (unsigned long)parser.lengthErrors(), (unsigned long)droppedBytes.load(),
      (unsigned long)udpErrors.load(), wifiConfigured ? "true" : "false", rssi, IP2STR(&ip.ip),
      (unsigned long)esp_get_free_heap_size());
  if (n > 0 && static_cast<size_t>(n) < sizeof(json)) sendUdp(json, n);
  ESP_LOGI(TAG, "%s", json);
}

static void dataTask(void*) {
  uint8_t buf[256];
  uint32_t generation = 0;
  int64_t lastStatus = 0;
  for (;;) {
    // Wakes as soon as any bytes arrive; the timeout only paces the status message.
    const size_t n = xStreamBufferReceive(rxStream, buf, sizeof(buf), pdMS_TO_TICKS(100));
    if (connGeneration != generation) {
      generation = connGeneration;
      parser.reset();
    }
    for (size_t i = 0; i < n; i++) {
      if (parser.feed(buf[i])) handlePacket();
    }
    publishStatus(nullptr);
    const int64_t now = esp_timer_get_time();
    if (now - lastStatus >= 1000000) {
      lastStatus = now;
      sendStatus();
    }
  }
}

// ---------------------------------------------------------------------------
// Serial console commands (one per line):
//   wifi-set<TAB><ssid><TAB><password>   save credentials to NVS and reconnect
//   wifi-clear                           erase credentials
//   ble-disconnect                       drop the BLE link (it reconnects by itself)

static void handleCommand(char* line) {
  if (strncmp(line, "wifi-set\t", 9) == 0) {
    char* ssid = line + 9;
    char* tab = strchr(ssid, '\t');
    if (!tab || tab == ssid) {
      printf("ERR usage: wifi-set<TAB>ssid<TAB>password\n");
      return;
    }
    *tab = '\0';
    if (const char* err = saveWifiCredentials(ssid, tab + 1)) {
      printf("ERR %s\n", err);
      return;
    }
    printf("OK wifi saved\n");
    applyWifiConfig();
  } else if (strcmp(line, "wifi-clear") == 0) {
    clearWifiCredentials();
    printf("OK wifi cleared\n");
    applyWifiConfig();
  } else if (strcmp(line, "ble-disconnect") == 0) {
    if (bleState == BleState::Connected) {
      disconnect();
      printf("OK ble disconnecting\n");
    } else {
      printf("ERR ble not connected\n");
    }
  } else if (line[0]) {
    printf("ERR unknown command\n");
  }
}

static void consoleTask(void*) {
  char line[200];
  size_t len = 0;
  for (;;) {
    uint8_t c;
    if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) <= 0) continue;
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = '\0';
      handleCommand(line);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = static_cast<char>(c);
    }
  }
}

static void initConsole() {
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(usb_serial_jtag_driver_install(&cfg));
  usb_serial_jtag_vfs_use_driver();
}

// ---------------------------------------------------------------------------

extern "C" void app_main() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);

  esp_pm_config_t pm = {};
  pm.max_freq_mhz = CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
  pm.min_freq_mhz = CONFIG_RACEBOX_CPU_MIN_MHZ;
  pm.light_sleep_enable = false;
  ESP_ERROR_CHECK(esp_pm_configure(&pm));

  initConsole();
  rxStream = xStreamBufferCreate(8192, 1);
  sdlog::init();  // before the frontend: the LCD shares its SPI bus
  bridge::frontendStart();
  initWifi();
  initBle();

  const esp_timer_create_args_t window = {.callback = closeConfigWindow, .name = "config_window"};
  esp_timer_handle_t windowTimer;
  ESP_ERROR_CHECK(esp_timer_create(&window, &windowTimer));
  ESP_ERROR_CHECK(esp_timer_start_once(windowTimer, CONFIG_RACEBOX_CONFIG_WINDOW_S * 1000000LL));

  xTaskCreate(dataTask, "data", 6144, nullptr, 10, nullptr);
  xTaskCreate(consoleTask, "console", 4096, nullptr, 2, nullptr);
}

void bridge::onLongPress() {
  if (sdlog::state() == sdlog::State::Safe) {
    sdlog::requestStart();
  } else {
    sdlog::requestStop();
  }
}
