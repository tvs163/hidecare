/*
 * Hidecare monitor-server client. See hidecare_client.h.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#include "hidecare_client.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <sys/time.h>
#include <mutex>
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
#include "runtime/esp_idf/csi_tap.h"
#include "vitals.h"
#include <mbedtls/base64.h>

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

// ---- ext v2: CSI band variance (filled by the Wi-Fi-context tap) ----------
constexpr int kBands = 16;  // 64 HT20 subcarriers / 4
portMUX_TYPE g_band_mux = portMUX_INITIALIZER_UNLOCKED;
struct BandAcc { uint32_t n; float sum[kBands], sumsq[kBands]; } g_band_acc{};

// ---- raw CSI capture (server-requested, labeled sessions) ------------------
constexpr int kCsiRing = 48;
constexpr int64_t kCsiMinGapUs = 50000;  // <= 20 frames/s go to the server
constexpr uint32_t kCsiMaxSec = 120;
struct CsiFrame { int64_t t_ms; int8_t rssi, noise; uint8_t ch; uint8_t len; int8_t iq[128]; };
CsiFrame g_csi_ring[kCsiRing];
int g_csi_head = 0, g_csi_count = 0;
uint32_t g_csi_seq = 0, g_csi_drops = 0;
int64_t g_csi_last_us = 0;
std::atomic<int> g_csi_session{0};
std::atomic<long long> g_csi_until{0};  // epoch seconds

void csi_tap(const int8_t *iq, uint16_t len, int8_t rssi, int8_t noise, uint8_t ch) {
  if (len < 128) return;
#if CONFIG_HIDECARE_VITALS_ENABLED
  vitals::feed(iq, len);
#endif
  float amp[kBands];
  for (int b = 0; b < kBands; b++) {
    float a = 0;
    for (int k = 0; k < 4; k++) {
      const int i = (b * 4 + k) * 2;
      a += std::sqrt(static_cast<float>(iq[i] * iq[i] + iq[i + 1] * iq[i + 1]));
    }
    amp[b] = a * 0.25f;
  }
  const bool cap = g_csi_session.load(std::memory_order_relaxed) != 0;
  const int64_t now_us = esp_timer_get_time();
  struct timeval tv;
  if (cap) gettimeofday(&tv, nullptr);
  portENTER_CRITICAL_SAFE(&g_band_mux);
  g_band_acc.n++;
  for (int b = 0; b < kBands; b++) { g_band_acc.sum[b] += amp[b]; g_band_acc.sumsq[b] += amp[b] * amp[b]; }
  if (cap && now_us - g_csi_last_us >= kCsiMinGapUs) {
    g_csi_last_us = now_us;
    if (g_csi_count == kCsiRing) { g_csi_drops++; g_csi_head = (g_csi_head + 1) % kCsiRing; g_csi_count--; }
    CsiFrame &f = g_csi_ring[(g_csi_head + g_csi_count) % kCsiRing];
    f.t_ms = static_cast<int64_t>(tv.tv_sec) * 1000 + tv.tv_usec / 1000;
    f.rssi = rssi; f.noise = noise; f.ch = ch; f.len = 128;
    memcpy(f.iq, iq, 128);
    g_csi_count++;
  }
  portEXIT_CRITICAL_SAFE(&g_band_mux);
}

// Quantize variance as round(16*log2(1+var)), 0..255.
uint8_t q_var(float v) {
  if (!(v > 0)) return 0;
  const float q = 16.0f * std::log2(1.0f + v);
  return q >= 255.0f ? 255 : static_cast<uint8_t>(q + 0.5f);
}

Diag g_diag{};  // latest, frontend task only

struct TelemetryWindow {
  time_t ts;         // epoch at window end, 0 if SNTP not synced yet
  uint32_t up_ms;    // uptime at window end (lets the server date ts=0 windows)
  float mvmt_avg, mvmt_max, motion_ratio, thr;
  int8_t rssi;
  char state[12];
  float mvmt_std, mvmt_min, mvmt_p90;  // ext v2
  uint8_t channel;
  Diag diag;
  bool has_bands;
  uint8_t bands[kBands];
  bool has_vitals;     // ext v3
  vitals::Snapshot vit;
};

// Ring buffer of finished windows, guarded by g_tel_mutex.
std::mutex g_tel_mutex;
TelemetryWindow g_tel_ring[CONFIG_HIDECARE_TELEMETRY_QUEUE];
size_t g_tel_head = 0, g_tel_count = 0;
uint32_t g_tel_dropped = 0;
// Current window accumulator (frontend task only).
struct {
  uint32_t start_ms = 0, n = 0, n_motion = 0;
  double sum = 0, sumsq = 0;
  float max = 0, min = 0, thr = 0;
  int rssi = 0;
  uint8_t channel = 0;
  float vals[64];
  char state[12] = "not_ready";
} g_acc;
std::atomic<bool> g_baby_sleep{false};
std::atomic<uint32_t> g_last_motion_sent_ms{0};
std::atomic<bool> g_any_motion_sent{false};
std::atomic<bool> g_last_sent_was_motion{false};

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

esp_err_t collect_body(esp_http_client_event_t *evt) {
  if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
    auto *out = static_cast<std::string *>(evt->user_data);
    if (out->size() < 512) out->append(static_cast<const char *>(evt->data), evt->data_len);
  }
  return ESP_OK;
}

// Persistent keep-alive client for /telemetry: one TLS handshake, then a small
// POST every window. Recreated after any transport error.
esp_http_client_handle_t g_tel_client = nullptr;
std::string g_tel_response;

int post_telemetry(const std::string &body) {
  if (g_tel_client == nullptr) {
    static std::string target = url("/telemetry");
    esp_http_client_config_t cfg{};
    cfg.url = target.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = true;
    cfg.event_handler = collect_body;
    cfg.user_data = &g_tel_response;
    g_tel_client = esp_http_client_init(&cfg);
    if (g_tel_client == nullptr) return -1;
    esp_http_client_set_header(g_tel_client, "Content-Type", "application/json");
  }
  g_tel_response.clear();
  esp_http_client_set_post_field(g_tel_client, body.data(), static_cast<int>(body.size()));
  int status = -1;
  if (esp_http_client_perform(g_tel_client) == ESP_OK) {
    status = esp_http_client_get_status_code(g_tel_client);
  } else {
    esp_http_client_cleanup(g_tel_client);
    g_tel_client = nullptr;
  }
  g_server_ok.store(status >= 200 && status < 300);
  return status;
}

void apply_mode_from_response(const std::string &resp) {
  const size_t k = resp.find("\"mode\"");
  if (k == std::string::npos) return;
  const bool baby = resp.find("baby_sleep", k) != std::string::npos &&
                    resp.find("baby_sleep", k) < k + 24;
  if (baby != g_baby_sleep.load()) {
    ESP_LOGI(TAG, "mode -> %s", baby ? "baby_sleep" : "normal");
    g_baby_sleep.store(baby);
  }
}

std::string fmt_float(float v) {
  char b[24];
  snprintf(b, sizeof(b), "%.5g", static_cast<double>(v));
  return b;
}

std::string ext_json(const TelemetryWindow &w) {
  std::string e = std::string("{\"v\":") + (w.has_vitals ? "3" : "2") + ",\"std\":" + fmt_float(w.mvmt_std) + ",\"min\":" + fmt_float(w.mvmt_min) +
                  ",\"p90\":" + fmt_float(w.mvmt_p90) + ",\"ch\":" + std::to_string(w.channel);
  if (w.diag.valid) {
    e += ",\"occ\":" + fmt_float(w.diag.occ) + ",\"cb\":" + fmt_float(w.diag.cb) + ",\"acc\":" + fmt_float(w.diag.acc) +
         ",\"hwerr\":" + fmt_float(w.diag.hwerr) + ",\"gen\":" + fmt_float(w.diag.gen) + ",\"rx\":" + fmt_float(w.diag.rx);
  }
  e += ",\"det\":\"" + std::string(w.diag.det[0] ? w.diag.det : "unknown") + "\"";
  if (w.has_bands) {
    char hex[kBands * 2 + 1];
    for (int i = 0; i < kBands; i++) snprintf(hex + i * 2, 3, "%02x", w.bands[i]);
    e += ",\"bands\":\"" + std::string(hex) + "\"";
  }
  if (w.has_vitals) {
    const vitals::Snapshot &v = w.vit;
    e += ",\"vit\":" + std::string(v.enabled ? "1" : "0") + ",\"fs\":" + fmt_float(v.fs_hz) +
         ",\"vrej\":" + std::to_string(v.rejected) + ",\"vdrop\":" + std::to_string(v.dropped) +
         ",\"ping\":" + std::to_string(v.ping_hz) + ",\"fall_phase\":" + (v.fall_phase ? "1" : "0");
    if (v.br_conf > 0) e += ",\"br_bpm\":" + fmt_float(v.br_bpm) + ",\"br_conf\":" + fmt_float(v.br_conf);
    if (v.hr_conf > 0) e += ",\"hr_bpm\":" + fmt_float(v.hr_bpm) + ",\"hr_conf\":" + fmt_float(v.hr_conf);
  }
  return e + "}";
}

// Capture request in the /telemetry reply: "capture":{"id":N,"label":"..","kind":"csi","until":T}
void apply_capture_from_response(const std::string &resp) {
  const size_t k = resp.find("\"capture\"");
  if (k == std::string::npos) {
    if (g_csi_session.exchange(0) != 0) ESP_LOGI(TAG, "csi capture off");
    return;
  }
  const size_t end = resp.find('}', k);
  const std::string c = resp.substr(k, end == std::string::npos ? std::string::npos : end - k);
  if (c.find("\"kind\":\"csi\"") == std::string::npos) { g_csi_session.store(0); return; }
  auto num = [&](const char *key) -> long long {
    const size_t p = c.find(key);
    return p == std::string::npos ? 0 : atoll(c.c_str() + p + strlen(key));
  };
  const int id = static_cast<int>(num("\"id\":"));
  long long until = num("\"until\":");
  const long long cap = static_cast<long long>(now_utc()) + kCsiMaxSec;
  if (until > cap) until = cap;
  if (id > 0 && g_csi_session.load() != id) ESP_LOGI(TAG, "csi capture session %d until %lld", id, until);
  g_csi_until.store(until);
  g_csi_session.store(id);
}

esp_http_client_handle_t g_csi_client = nullptr;
std::string g_csi_response;

// Send buffered raw CSI frames. Frames: {"ts","t_ms","seq","rssi","nf","ch","iq":base64(128 B int8 I/Q)}.
void flush_csi() {
  const int sid = g_csi_session.load();
  if (sid == 0) return;
  if (static_cast<long long>(now_utc()) > g_csi_until.load()) {
    g_csi_session.store(0);
    ESP_LOGI(TAG, "csi capture %d done (drops=%" PRIu32 ")", sid, g_csi_drops);
  }
  static CsiFrame batch[kCsiRing];
  int n = 0;
  portENTER_CRITICAL(&g_band_mux);
  while (g_csi_count > 0 && n < kCsiRing) {
    batch[n++] = g_csi_ring[g_csi_head];
    g_csi_head = (g_csi_head + 1) % kCsiRing;
    g_csi_count--;
  }
  portEXIT_CRITICAL(&g_band_mux);
  if (n == 0) return;
  std::string body;
  body.reserve(n * 260 + 80);
  body = "{\"device_code\":" + json_str(g_device_code) + ",\"session_id\":" + std::to_string(sid) + ",\"frames\":[";
  for (int i = 0; i < n; i++) {
    unsigned char b64[180];
    size_t ol = 0;
    mbedtls_base64_encode(b64, sizeof(b64), &ol, reinterpret_cast<const unsigned char *>(batch[i].iq), batch[i].len);
    if (i) body += ",";
    body += "{\"ts\":" + std::to_string(static_cast<long long>(batch[i].t_ms / 1000)) + ",\"t_ms\":" +
            std::to_string(static_cast<long long>(batch[i].t_ms)) + ",\"seq\":" + std::to_string(++g_csi_seq) +
            ",\"rssi\":" + std::to_string(batch[i].rssi) + ",\"nf\":" + std::to_string(batch[i].noise) +
            ",\"ch\":" + std::to_string(batch[i].ch) + ",\"iq\":\"" + std::string(reinterpret_cast<char *>(b64), ol) + "\"}";
  }
  body += "]}";
  if (g_csi_client == nullptr) {
    static std::string target = url("/csi");
    esp_http_client_config_t cfg = {};
    cfg.url = target.c_str();
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = kHttpTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.keep_alive_enable = true;
    cfg.event_handler = collect_body;
    cfg.user_data = &g_csi_response;
    g_csi_client = esp_http_client_init(&cfg);
    if (g_csi_client == nullptr) return;
    esp_http_client_set_header(g_csi_client, "Content-Type", "application/json");
  }
  g_csi_response.clear();
  esp_http_client_set_post_field(g_csi_client, body.data(), static_cast<int>(body.size()));
  int status = -1;
  if (esp_http_client_perform(g_csi_client) == ESP_OK) {
    status = esp_http_client_get_status_code(g_csi_client);
  } else {
    esp_http_client_cleanup(g_csi_client);
    g_csi_client = nullptr;
  }
  if (status == 409) g_csi_session.store(0);  // session over on the server
  if (status < 200 || status >= 300) ESP_LOGW(TAG, "csi upload %d frames: %d", n, status);
}

// Send up to kBatch buffered windows. True if the buffer was (partly) drained
// or empty; false on a transport/server error (keep the windows).
constexpr size_t kTelBatch = 30;
bool flush_telemetry() {
  static TelemetryWindow batch[kTelBatch];  // client task only; keeps the 8 KB stack free
  size_t n = 0;
  uint32_t dropped;
  {
    std::lock_guard<std::mutex> lock(g_tel_mutex);
    const size_t cap = CONFIG_HIDECARE_TELEMETRY_QUEUE;
    const size_t tail = (g_tel_head + cap - g_tel_count) % cap;
    n = g_tel_count < kTelBatch ? g_tel_count : kTelBatch;
    for (size_t i = 0; i < n; i++) batch[i] = g_tel_ring[(tail + i) % cap];
    dropped = g_tel_dropped;
  }
  if (n == 0) return true;
  std::string body = "{\"device_code\":" + json_str(g_device_code) +
                     ",\"fw_version\":" + json_str(espectre::frontend_firmware_version()) +
                     ",\"up_ms\":" + std::to_string(now_ms()) +
                     ",\"dropped\":" + std::to_string(dropped) + ",\"samples\":[";
  for (size_t i = 0; i < n; i++) {
    const TelemetryWindow &w = batch[i];
    if (i) body += ",";
    body += "{\"ts\":" + std::to_string(static_cast<long long>(w.ts)) +
            ",\"up_ms\":" + std::to_string(w.up_ms) +
            ",\"mvmt_avg\":" + fmt_float(w.mvmt_avg) + ",\"mvmt_max\":" + fmt_float(w.mvmt_max) +
            ",\"motion_ratio\":" + fmt_float(w.motion_ratio) + ",\"thr\":" + fmt_float(w.thr) +
            ",\"rssi\":" + std::to_string(w.rssi) + ",\"state\":\"" + w.state + "\"" +
            ",\"ext\":" + ext_json(w) + "}";
  }
  body += "]}";
  const int status = post_telemetry(body);
  if (status >= 200 && status < 300) { apply_mode_from_response(g_tel_response); apply_capture_from_response(g_tel_response);
#if CONFIG_HIDECARE_VITALS_ENABLED
    vitals::apply_config_json(g_tel_response);
#endif
  }
  if ((status >= 200 && status < 300) || (status >= 400 && status < 500 && status != 429)) {
    if (status >= 400) ESP_LOGW(TAG, "telemetry rejected: %d", status);
    std::lock_guard<std::mutex> lock(g_tel_mutex);
    g_tel_count = g_tel_count > n ? g_tel_count - n : 0;
    g_tel_dropped = 0;
    return true;
  }
  ESP_LOGW(TAG, "telemetry failed: status=%d", status);
  return false;
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
  if (status >= 200 && status < 300) {
    if (ev.kind == EventKind::MOTION) g_last_motion_sent_ms.store(now_ms());
    return true;
  }
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
  uint32_t next_telemetry = now_ms();
  bool sntp_started = false;
  bool pending_valid = false;
  QueuedEvent pending{};

  while (true) {
    if (!wifi_connected()) {
      registered = false;  // re-register after every reconnect
      if (g_tel_client != nullptr) { esp_http_client_cleanup(g_tel_client); g_tel_client = nullptr; }
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
    if (static_cast<int32_t>(now - next_telemetry) >= 0) {
      next_telemetry = now + (flush_telemetry() ? CONFIG_HIDECARE_TELEMETRY_SEC * 1000U : 5000U);
    }
    {
      static uint32_t next_csi = 0;
      if (g_csi_session.load() != 0 && static_cast<int32_t>(now - next_csi) >= 0) {
        next_csi = now + 1000;
        flush_csi();
      }
    }
    if (!pending_valid && xQueueReceive(g_queue, &pending, pdMS_TO_TICKS(200)) == pdTRUE) {
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
  espectre::set_csi_tap(csi_tap);
#if CONFIG_HIDECARE_VITALS_ENABLED
  if (!vitals::start()) ESP_LOGW(TAG, "vitals task not started");
#endif
  return xTaskCreate(client_task, "hidecare", 8192, nullptr, 3, nullptr) == pdPASS;
}

bool baby_sleep_mode() { return g_baby_sleep.load(); }

void set_diag(const Diag &d) { g_diag = d; }

// Debounce (normal mode only): a motion event within DEBOUNCE_SEC of the last
// one is dropped, and so is the no_motion that would close it, so the server
// never sees two no_motion in a row.
void notify_motion(bool motion) {
  if (motion) {
    const uint32_t now = now_ms();
    if (!g_baby_sleep.load() && g_any_motion_sent.load() &&
        now - g_last_motion_sent_ms.load() < CONFIG_HIDECARE_MOTION_DEBOUNCE_SEC * 1000U) {
      return;
    }
    g_any_motion_sent.store(true);
    g_last_motion_sent_ms.store(now);
    g_last_sent_was_motion.store(true);
    enqueue(EventKind::MOTION);
  } else {
    if (!g_last_sent_was_motion.exchange(false)) return;
    enqueue(EventKind::NO_MOTION);
  }
}

void add_sample(float mvmt, float thr, bool motion, int rssi, const char *state, uint8_t channel) {
  if (g_queue == nullptr) return;
  const uint32_t now = now_ms();
  if (g_acc.n == 0) g_acc.start_ms = now;
  if (mvmt < 0 || mvmt != mvmt) mvmt = 0;
  g_acc.n++;
  g_acc.sum += mvmt;
  g_acc.sumsq += static_cast<double>(mvmt) * mvmt;
  if (mvmt > g_acc.max) g_acc.max = mvmt;
  if (g_acc.n == 1 || mvmt < g_acc.min) g_acc.min = mvmt;
  if (channel) g_acc.channel = channel;
  if (g_acc.n <= 64) g_acc.vals[g_acc.n - 1] = mvmt;
  if (motion) g_acc.n_motion++;
  g_acc.thr = thr;
  if (rssi != INT8_MIN) g_acc.rssi = rssi;
  strlcpy(g_acc.state, state, sizeof(g_acc.state));
  if (now - g_acc.start_ms < CONFIG_HIDECARE_TELEMETRY_SEC * 1000U) return;
  TelemetryWindow w{};
  w.ts = now_utc();
  w.up_ms = now;
  w.mvmt_avg = static_cast<float>(g_acc.sum / g_acc.n);
  w.mvmt_max = g_acc.max;
  w.motion_ratio = static_cast<float>(g_acc.n_motion) / static_cast<float>(g_acc.n);
  w.thr = g_acc.thr;
  w.rssi = static_cast<int8_t>(g_acc.rssi);
  strlcpy(w.state, g_acc.state, sizeof(w.state));
  {
    const double mean = g_acc.sum / g_acc.n;
    const double var = g_acc.sumsq / g_acc.n - mean * mean;
    w.mvmt_std = static_cast<float>(var > 0 ? std::sqrt(var) : 0.0);
  }
  w.mvmt_min = g_acc.min;
  w.channel = g_acc.channel;
  {
    const uint32_t m = g_acc.n < 64 ? g_acc.n : 64;
    const uint32_t k = (m * 9) / 10 < m ? (m * 9) / 10 : m - 1;
    std::nth_element(g_acc.vals, g_acc.vals + k, g_acc.vals + m);
    w.mvmt_p90 = g_acc.vals[k];
  }
  w.diag = g_diag;
  {
    BandAcc b;
    portENTER_CRITICAL(&g_band_mux);
    b = g_band_acc;
    g_band_acc = BandAcc{};
    portEXIT_CRITICAL(&g_band_mux);
    w.has_bands = b.n >= 2;
    for (int i = 0; i < kBands && w.has_bands; i++) {
      const float mean = b.sum[i] / b.n;
      w.bands[i] = q_var(b.sumsq[i] / b.n - mean * mean);
    }
  }
#if CONFIG_HIDECARE_VITALS_ENABLED
  w.vit = vitals::take();
  w.has_vitals = true;
#endif
  g_acc.n = g_acc.n_motion = 0;
  g_acc.sum = g_acc.sumsq = 0;
  g_acc.max = 0;
  std::lock_guard<std::mutex> lock(g_tel_mutex);
  const size_t cap = CONFIG_HIDECARE_TELEMETRY_QUEUE;
  g_tel_ring[g_tel_head] = w;
  g_tel_head = (g_tel_head + 1) % cap;
  if (g_tel_count < cap) g_tel_count++; else g_tel_dropped++;  // overwrite oldest
}

}  // namespace hidecare

#else  // !CONFIG_HIDECARE_ENABLED

namespace hidecare {
std::string device_code() { return {}; }
std::string device_suffix() { return {}; }
bool server_ok() { return false; }
bool start() { return true; }
void notify_motion(bool) {}
void add_sample(float, float, bool, int, const char *, uint8_t) {}
void set_diag(const Diag &) {}
bool baby_sleep_mode() { return false; }
}  // namespace hidecare

#endif
