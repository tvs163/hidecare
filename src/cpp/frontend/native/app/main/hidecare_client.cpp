#include <atomic>
#include <mutex>
/*
 * Hidecare monitor-server client. See hidecare_client.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "hidecare_client.h"

#include <cctype>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <strings.h>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_netif.h>
#include <esp_netif_sntp.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/task.h>
#include <sdkconfig.h>

#include "frontend_firmware_version.h"

#if CONFIG_HIDECARE_ENABLED

namespace hidecare {
namespace {

const char *const TAG = "hidecare";

constexpr uint32_t kRegisterBackoffMinMs = 5 * 1000;
constexpr uint32_t kRegisterBackoffMaxMs = 10 * 60 * 1000;
constexpr int kHttpTimeoutMs = 10000;
constexpr time_t kValidEpoch = 1700000000;  // 2023-11; anything earlier means SNTP has not synced

enum class EventKind : uint8_t { MOTION, NO_MOTION, HEARTBEAT };

struct QueuedEvent {
  EventKind kind;
  time_t timestamp;  // 0 when time was unknown at capture
};

QueueHandle_t g_queue = nullptr;
std::string g_device_code;
std::string g_mac;
std::atomic<bool> g_server_ok{false};

const char *event_name(EventKind kind) {
  switch (kind) {
    case EventKind::MOTION: return "motion";
    case EventKind::NO_MOTION: return "no_motion";
    case EventKind::HEARTBEAT: return "heartbeat";
  }
  return "heartbeat";
}

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

bool time_valid(time_t t) { return t >= kValidEpoch; }

time_t now_utc() {
  const time_t t = time(nullptr);
  return time_valid(t) ? t : 0;
}

std::string iso_utc(time_t t) {
  struct tm tm_utc {};
  gmtime_r(&t, &tm_utc);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm_utc);
  return buf;
}

bool wifi_connected() {
  esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip{};
  return netif != nullptr && esp_netif_get_ip_info(netif, &ip) == ESP_OK && ip.ip.addr != 0;
}

std::string url(const char *path) { return std::string(CONFIG_HIDECARE_SERVER_URL) + path; }

// Escape for a JSON string value. Inputs here are device codes and versions.
std::string json_str(const std::string &in) {
  std::string out = "\"";
  for (char c : in) {
    if (c == '"' || c == '\\') out += '\\';
    if (static_cast<unsigned char>(c) < 0x20) continue;
    out += c;
  }
  return out + "\"";
}

/** POST JSON, return HTTP status (or -1 on transport error). */
int post_json(const char *path, const std::string &body, std::string *response = nullptr) {
  const std::string target = url(path);
  esp_http_client_config_t cfg{};
  cfg.url = target.c_str();
  cfg.method = HTTP_METHOD_POST;
  cfg.timeout_ms = kHttpTimeoutMs;
  cfg.crt_bundle_attach = esp_crt_bundle_attach;
  esp_http_client_handle_t client = esp_http_client_init(&cfg);
  if (client == nullptr) return -1;
  esp_http_client_set_header(client, "Content-Type", "application/json");
  int status = -1;
  if (esp_http_client_open(client, static_cast<int>(body.size())) == ESP_OK) {
    if (esp_http_client_write(client, body.data(), static_cast<int>(body.size())) ==
        static_cast<int>(body.size())) {
      esp_http_client_fetch_headers(client);
      status = esp_http_client_get_status_code(client);
      if (response != nullptr) {
        char buf[256];
        int n;
        while ((n = esp_http_client_read(client, buf, sizeof(buf))) > 0 && response->size() < 1024) {
          response->append(buf, n);
        }
      }
    }
  }
  esp_http_client_cleanup(client);
  g_server_ok.store(status >= 200 && status < 300);
  return status;
}

bool register_device() {
  const std::string body = "{\"device_code\":" + json_str(g_device_code) +
                           ",\"fw_version\":" + json_str(espectre::frontend_firmware_version()) +
                           ",\"mac\":" + json_str(g_mac) + "}";
  std::string response;
  const int status = post_json("/devices/register", body, &response);
  if (status == 200) {
    ESP_LOGI(TAG, "registered as %s: %s", g_device_code.c_str(), response.c_str());
    return true;
  }
  ESP_LOGW(TAG, "register failed: status=%d", status);
  return false;
}

bool send_event(const QueuedEvent &ev) {
  std::string body = "{\"device_code\":" + json_str(g_device_code) +
                     ",\"event\":\"" + event_name(ev.kind) + "\"";
  if (time_valid(ev.timestamp)) body += ",\"timestamp\":\"" + iso_utc(ev.timestamp) + "\"";
  body += "}";
  const int status = post_json("/events", body);
  if (status >= 200 && status < 300) return true;
  ESP_LOGW(TAG, "event %s failed: status=%d", event_name(ev.kind), status);
  // 4xx means the server rejected the payload; retrying will not help.
  return status >= 400 && status < 500;
}

void enqueue(EventKind kind) {
  if (g_queue == nullptr) return;
  const QueuedEvent ev{kind, now_utc()};
  if (xQueueSend(g_queue, &ev, 0) != pdTRUE) {
    QueuedEvent dropped;
    (void) xQueueReceive(g_queue, &dropped, 0);  // keep the newest state
    (void) xQueueSend(g_queue, &ev, 0);
  }
}


void client_task(void *) {
  bool registered = false;
  uint32_t backoff = kRegisterBackoffMinMs;
  uint32_t next_register = 0;
  uint32_t next_heartbeat = now_ms() + CONFIG_HIDECARE_HEARTBEAT_SEC * 1000U;
  bool sntp_started = false;
  bool pending_valid = false;
  QueuedEvent pending{};

  while (true) {
    if (!wifi_connected()) {
      registered = false;  // re-register after every reconnect
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    if (!sntp_started) {
      esp_sntp_config_t sntp = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
      sntp_started = esp_netif_sntp_init(&sntp) == ESP_OK;
    }
    const uint32_t now = now_ms();
    if (!registered) {
      if (static_cast<int32_t>(now - next_register) >= 0) {
        registered = register_device();
        if (registered) {
          backoff = kRegisterBackoffMinMs;
        } else {
          next_register = now + backoff;
          backoff = backoff * 2 > kRegisterBackoffMaxMs ? kRegisterBackoffMaxMs : backoff * 2;
        }
      }
      if (!registered) { vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    }
    if (static_cast<int32_t>(now - next_heartbeat) >= 0) {
      next_heartbeat = now + CONFIG_HIDECARE_HEARTBEAT_SEC * 1000U;
      enqueue(EventKind::HEARTBEAT);
    }
    if (!pending_valid && xQueueReceive(g_queue, &pending, pdMS_TO_TICKS(1000)) == pdTRUE) {
      if (!time_valid(pending.timestamp)) pending.timestamp = 0;
      pending_valid = true;
    }
    if (pending_valid) {
      if (send_event(pending)) {
        pending_valid = false;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5000));  // server down: keep the event and retry
      }
    }
  }
}

}  // namespace

void compute_device_code();

std::string device_code() {
  // Computed once; both the code and the setup AP name derive from the
  // station MAC (ESP_MAC_WIFI_STA), never the SoftAP/BT MAC.
  static std::once_flag once;
  std::call_once(once, compute_device_code);
  return g_device_code;
}

void compute_device_code() {
  uint8_t mac[6] = {};
  esp_read_mac(mac, ESP_MAC_WIFI_STA);
  char buf[24];
  snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  g_mac = buf;
  if (CONFIG_HIDECARE_DEVICE_CODE[0] != '\0') {
    g_device_code = CONFIG_HIDECARE_DEVICE_CODE;
  } else {
    snprintf(buf, sizeof(buf), "HC-%02X%02X%02X", mac[3], mac[4], mac[5]);
    g_device_code = buf;
  }
}

std::string device_suffix() {
  const std::string code = device_code();
  return code.rfind("HC-", 0) == 0 ? code.substr(3) : code;
}

bool server_ok() { return g_server_ok.load(); }

bool start() {
  if (g_queue != nullptr) return true;
  ESP_LOGI(TAG, "device_code=%s mac=%s server=%s", device_code().c_str(), g_mac.c_str(), CONFIG_HIDECARE_SERVER_URL);
  g_queue = xQueueCreate(16, sizeof(QueuedEvent));
  if (g_queue == nullptr) return false;
  return xTaskCreate(client_task, "hidecare", 8192, nullptr, 3, nullptr) == pdPASS;
}

void notify_motion(bool motion) { enqueue(motion ? EventKind::MOTION : EventKind::NO_MOTION); }

}  // namespace hidecare

#else  // !CONFIG_HIDECARE_ENABLED

namespace hidecare {
std::string device_code() { return {}; }
std::string device_suffix() { return {}; }
bool server_ok() { return false; }
bool start() { return true; }
void notify_motion(bool) {}
}  // namespace hidecare

#endif
