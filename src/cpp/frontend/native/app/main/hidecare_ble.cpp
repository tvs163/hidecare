/*
 * Improv Wi-Fi over BLE (https://www.improv-wifi.com/ble/) on NimBLE.
 * Runs only while setup mode is on; stop() deinitialises NimBLE to give the
 * heap back.
 */
#include "hidecare_setup.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "improv.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "frontend_firmware_version.h"
#include "hidecare_client.h"

namespace hidecare::setup::ble {
namespace {

const char *TAG = "hidecare.ble";

// 00467768-6228-2272-4663-2774782680XX, little-endian.
#define IMPROV_UUID(x) \
  BLE_UUID128_INIT(x, 0x80, 0x26, 0x78, 0x74, 0x27, 0x63, 0x46, 0x72, 0x22, 0x28, 0x62, 0x68, 0x77, 0x46, 0x00)
const ble_uuid128_t kSvc = IMPROV_UUID(0x00);
const ble_uuid128_t kState = IMPROV_UUID(0x01);
const ble_uuid128_t kError = IMPROV_UUID(0x02);
const ble_uuid128_t kRpc = IMPROV_UUID(0x03);
const ble_uuid128_t kResult = IMPROV_UUID(0x04);
const ble_uuid128_t kCaps = IMPROV_UUID(0x05);

std::mutex g_mtx;
uint8_t g_state = improv::STATE_AUTHORIZED;
uint8_t g_error = improv::ERROR_NONE;
std::vector<uint8_t> g_result;
std::vector<uint8_t> g_rx;
uint16_t g_state_h = 0, g_error_h = 0, g_result_h = 0;
std::atomic<bool> g_running{false};
std::atomic<bool> g_synced{false};
std::string g_name;
uint8_t g_own_addr_type = 0;

void notify(uint16_t handle) {
  if (g_synced.load() && handle != 0) ble_gatts_chr_updated(handle);
}

void set_state(uint8_t state) {
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_state = state;
  }
  notify(g_state_h);
}

void set_error(uint8_t error) {
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_error = error;
  }
  notify(g_error_h);
}

void set_result(std::vector<uint8_t> result) {
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_result = std::move(result);
  }
  notify(g_result_h);
}

void handle_rpc(const std::vector<uint8_t> &packet) {
  improv::ImprovCommand cmd = improv::parse_improv_data(packet.data(), packet.size(), true);
  switch (cmd.command) {
    case improv::WIFI_SETTINGS:
      set_error(improv::ERROR_NONE);
      if (!queue_connect(cmd.ssid, cmd.password)) set_error(improv::ERROR_UNABLE_TO_CONNECT);
      break;
    case improv::GET_DEVICE_INFO:
      set_result(improv::build_rpc_response(
          improv::GET_DEVICE_INFO,
          std::vector<std::string>{"Hidecare", espectre::frontend_firmware_version(), CONFIG_IDF_TARGET,
                                   hidecare::device_code()},
          true));
      break;
    case improv::BAD_CHECKSUM:
      set_error(improv::ERROR_INVALID_RPC);
      break;
    default:
      set_error(improv::ERROR_UNKNOWN_RPC);
      break;
  }
}

int access_cb(uint16_t, uint16_t, ble_gatt_access_ctxt *ctxt, void *arg) {
  const uintptr_t which = reinterpret_cast<uintptr_t>(arg);
  if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
    std::lock_guard<std::mutex> lock(g_mtx);
    switch (which) {
      case 1: return os_mbuf_append(ctxt->om, &g_state, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
      case 2: return os_mbuf_append(ctxt->om, &g_error, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
      case 4:
        return os_mbuf_append(ctxt->om, g_result.data(), g_result.size()) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
      case 5: {
        const uint8_t caps = 0;  // no identify
        return os_mbuf_append(ctxt->om, &caps, 1) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
      }
      default: return BLE_ATT_ERR_UNLIKELY;
    }
  }
  if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR && which == 3) {
    uint8_t buf[256];
    uint16_t len = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    std::vector<uint8_t> packet;
    {
      std::lock_guard<std::mutex> lock(g_mtx);
      g_rx.insert(g_rx.end(), buf, buf + len);
      // [command, length, data..., checksum]
      if (g_rx.size() >= 2 && g_rx.size() >= static_cast<size_t>(g_rx[1]) + 3U) {
        packet.assign(g_rx.begin(), g_rx.begin() + g_rx[1] + 3);
        g_rx.clear();
      } else if (g_rx.size() > 300) {
        g_rx.clear();
      }
    }
    if (!packet.empty()) handle_rpc(packet);
    return 0;
  }
  return BLE_ATT_ERR_UNLIKELY;
}

const ble_gatt_chr_def kChars[] = {
    {.uuid = &kState.u, .access_cb = access_cb, .arg = reinterpret_cast<void *>(1),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_state_h},
    {.uuid = &kError.u, .access_cb = access_cb, .arg = reinterpret_cast<void *>(2),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_error_h},
    {.uuid = &kRpc.u, .access_cb = access_cb, .arg = reinterpret_cast<void *>(3),
     .flags = BLE_GATT_CHR_F_WRITE},
    {.uuid = &kResult.u, .access_cb = access_cb, .arg = reinterpret_cast<void *>(4),
     .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY, .val_handle = &g_result_h},
    {.uuid = &kCaps.u, .access_cb = access_cb, .arg = reinterpret_cast<void *>(5), .flags = BLE_GATT_CHR_F_READ},
    {},
};

const ble_gatt_svc_def kServices[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &kSvc.u, .characteristics = kChars},
    {},
};

void advertise();

int gap_event(ble_gap_event *event, void *) {
  switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
      if (event->connect.status != 0) advertise();
      break;
    case BLE_GAP_EVENT_DISCONNECT:
    case BLE_GAP_EVENT_ADV_COMPLETE:
      if (g_running.load()) advertise();
      break;
    default:
      break;
  }
  return 0;
}

void advertise() {
  ble_hs_adv_fields fields{};
  fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
  fields.uuids128 = &kSvc;
  fields.num_uuids128 = 1;
  fields.uuids128_is_complete = 1;
  if (ble_gap_adv_set_fields(&fields) != 0) return;

  uint8_t svc_data[8] = {0x77, 0x46, 0, 0, 0, 0, 0, 0};  // UUID 0x4677 + state + caps
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    svc_data[2] = g_state;
  }
  ble_hs_adv_fields rsp{};
  rsp.name = reinterpret_cast<const uint8_t *>(g_name.c_str());
  rsp.name_len = g_name.size();
  rsp.name_is_complete = 1;
  rsp.svc_data_uuid16 = svc_data;
  rsp.svc_data_uuid16_len = sizeof(svc_data);
  ble_gap_adv_rsp_set_fields(&rsp);

  ble_gap_adv_params params{};
  params.conn_mode = BLE_GAP_CONN_MODE_UND;
  params.disc_mode = BLE_GAP_DISC_MODE_GEN;
  const int rc = ble_gap_adv_start(g_own_addr_type, nullptr, BLE_HS_FOREVER, &params, gap_event, nullptr);
  if (rc != 0 && rc != BLE_HS_EALREADY) ESP_LOGW(TAG, "adv start failed: %d", rc);
}

void on_sync() {
  ble_hs_id_infer_auto(0, &g_own_addr_type);
  g_synced.store(true);
  advertise();
  ESP_LOGI(TAG, "Improv BLE advertising as %s", g_name.c_str());
}

void host_task(void *) {
  nimble_port_run();
  nimble_port_freertos_deinit();
}

}  // namespace

bool start(const std::string &name) {
  if (g_running.load()) return true;
  g_name = name;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_state = improv::STATE_AUTHORIZED;
    g_error = improv::ERROR_NONE;
    g_result.clear();
    g_rx.clear();
  }
  if (nimble_port_init() != ESP_OK) {
    ESP_LOGE(TAG, "nimble_port_init failed");
    return false;
  }
  ble_hs_cfg.sync_cb = on_sync;
  ble_svc_gap_init();
  ble_svc_gatt_init();
  if (ble_gatts_count_cfg(kServices) != 0 || ble_gatts_add_svcs(kServices) != 0) {
    ESP_LOGE(TAG, "GATT setup failed");
    nimble_port_deinit();
    return false;
  }
  ble_svc_gap_device_name_set(g_name.c_str());
  g_running.store(true);
  nimble_port_freertos_init(host_task);
  return true;
}

void stop() {
  if (!g_running.exchange(false)) return;
  g_synced.store(false);
  ble_gap_adv_stop();
  if (nimble_port_stop() == 0) {
    nimble_port_deinit();
  }
  ESP_LOGI(TAG, "Improv BLE stopped");
}

bool running() { return g_running.load(); }

void on_connect_started() {
  if (g_running.load()) set_state(improv::STATE_PROVISIONING);
}

void on_connect_finished(bool ok) {
  if (!g_running.load()) return;
  if (ok) {
    set_state(improv::STATE_PROVISIONED);
    set_result(improv::build_rpc_response(improv::WIFI_SETTINGS, std::vector<std::string>{bot_url()}, true));
  } else {
    set_error(improv::ERROR_UNABLE_TO_CONNECT);
    set_state(improv::STATE_AUTHORIZED);
  }
}

}  // namespace hidecare::setup::ble

#else  // !CONFIG_BT_NIMBLE_ENABLED

namespace hidecare::setup::ble {
bool start(const std::string &) { return false; }
void stop() {}
bool running() { return false; }
void on_connect_started() {}
void on_connect_finished(bool) {}
}  // namespace hidecare::setup::ble

#endif
