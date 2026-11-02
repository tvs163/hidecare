// Hidecare vitals glue: SPSC frame ring fed by the CSI tap, a low-priority DSP
// task, optional gateway self-ping, server-driven runtime toggles.
#include "vitals.h"
#include "vitals_core.h"
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ping/ping_sock.h"
#include "sdkconfig.h"

namespace hidecare::vitals {
namespace {
const char *const TAG = "vitals";
constexpr int kRing = 32;  // ~0.3 s at 100 Hz
struct Frame { int64_t t_us; int8_t iq[128]; };
Frame g_ring[kRing];
std::atomic<uint32_t> g_w{0}, g_r{0}, g_dropped{0};
portMUX_TYPE g_feed_mux = portMUX_INITIALIZER_UNLOCKED;  // tap may run on either core

std::atomic<bool> g_en{CONFIG_HIDECARE_VITALS_DEFAULT_ON}, g_hr{true}, g_san{true};
std::atomic<int> g_fall_x10{150};
std::atomic<int> g_ping_hz{CONFIG_HIDECARE_VITALS_PING_HZ};
std::atomic<bool> g_cfg_dirty{true};

std::mutex g_out_mutex;
vc_output_t g_out{};
bool g_fall_latch = false;

vc_state_t g_state;  // ~5 KB, DSP task only

#if CONFIG_HIDECARE_VITALS_SELFPING
esp_ping_handle_t g_ping = nullptr;
int g_ping_running_hz = 0;
void ping_apply(int hz) {
  if (hz == g_ping_running_hz) return;
  if (g_ping) { esp_ping_stop(g_ping); esp_ping_delete_session(g_ping); g_ping = nullptr; }
  g_ping_running_hz = 0;
  if (hz <= 0) return;
  esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
  esp_netif_ip_info_t ip{};
  if (!sta || esp_netif_get_ip_info(sta, &ip) != ESP_OK || ip.gw.addr == 0) return;  // retry next tick
  esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
  pc.count = ESP_PING_COUNT_INFINITE;
  pc.interval_ms = 1000 / hz;
  pc.timeout_ms = 500;
  pc.data_size = 8;
  pc.task_stack_size = 2560;
  pc.task_prio = 1;
  ip4_addr_t gw;
  gw.addr = ip.gw.addr;
  ip_addr_copy_from_ip4(pc.target_addr, gw);
  esp_ping_callbacks_t cbs{};
  if (esp_ping_new_session(&pc, &cbs, &g_ping) == ESP_OK && esp_ping_start(g_ping) == ESP_OK) {
    g_ping_running_hz = hz;
    ESP_LOGI(TAG, "self-ping gateway at %d Hz", hz);
  } else if (g_ping) { esp_ping_delete_session(g_ping); g_ping = nullptr; }
}
#endif

void reconfigure() {
  vc_config_t c{};
  c.phase_sanitize = g_san.load();
  c.hr_enabled = g_hr.load();
  c.fall_thresh = g_fall_x10.load() / 10.0f;
  vc_init(&g_state, &c, 20.0f);
  g_r.store(g_w.load());  // drop stale frames
}

void task(void *) {
  int64_t last_est = 0;
#if CONFIG_HIDECARE_VITALS_SELFPING
  int64_t last_ping_try = 0;
#endif
  for (;;) {
    if (g_cfg_dirty.exchange(false)) reconfigure();
#if CONFIG_HIDECARE_VITALS_SELFPING
    const int64_t now = esp_timer_get_time();
    if (now - last_ping_try > 5000000) { last_ping_try = now; ping_apply(g_en.load() ? g_ping_hz.load() : 0); }
#endif
    if (!g_en.load()) { g_r.store(g_w.load()); vTaskDelay(pdMS_TO_TICKS(500)); continue; }
    int batch = 0;
    while (g_r.load(std::memory_order_relaxed) != g_w.load(std::memory_order_acquire) && batch < 16) {
      const Frame &f = g_ring[g_r.load() % kRing];
      vc_process(&g_state, f.iq, 64, f.t_us);
      g_r.fetch_add(1, std::memory_order_release);
      batch++;
    }
    const int64_t t = esp_timer_get_time();
    if (t - last_est >= 1000000) {
      last_est = t;
      vc_estimate(&g_state);  // ~256*lags MACs, a few ms
      std::lock_guard<std::mutex> lk(g_out_mutex);
      g_out = g_state.out;
      if (g_state.out.fall) { g_fall_latch = true; g_state.out.fall = false; }
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

bool json_num(const std::string &s, size_t from, size_t to, const char *key, float *out) {
  const std::string k = std::string("\"") + key + "\":";
  const size_t p = s.find(k, from);
  if (p == std::string::npos || p >= to) return false;
  *out = strtof(s.c_str() + p + k.size(), nullptr);
  return true;
}
}  // namespace

bool start() {
  return xTaskCreatePinnedToCore(task, "vitals", 4096, nullptr, 2, nullptr, 1) == pdPASS;
}

void feed(const int8_t *iq, uint16_t len) {
  if (len < 128 || !g_en.load(std::memory_order_relaxed)) return;
  portENTER_CRITICAL_SAFE(&g_feed_mux);
  const uint32_t w = g_w.load(std::memory_order_relaxed);
  if (w - g_r.load(std::memory_order_acquire) >= kRing) {
    g_dropped.fetch_add(1, std::memory_order_relaxed);
  } else {
    Frame &f = g_ring[w % kRing];
    f.t_us = esp_timer_get_time();
    memcpy(f.iq, iq, 128);
    g_w.store(w + 1, std::memory_order_release);
  }
  portEXIT_CRITICAL_SAFE(&g_feed_mux);
}

Snapshot take() {
  Snapshot s{};
  s.enabled = g_en.load();
  s.ping_hz = static_cast<uint16_t>(g_ping_hz.load());
  s.dropped = g_dropped.exchange(0);
  std::lock_guard<std::mutex> lk(g_out_mutex);
  s.br_bpm = g_out.br_bpm; s.hr_bpm = g_out.hr_bpm;
  s.br_conf = g_out.br_conf; s.hr_conf = g_out.hr_conf; s.fs_hz = g_out.fs_hz;
  s.frames = g_out.frames; s.rejected = g_out.rejected;
  s.fall_phase = g_fall_latch;
  g_fall_latch = false;
  return s;
}

void apply_config_json(const std::string &r) {
  const size_t k = r.find("\"vitals\":{");
  if (k == std::string::npos) return;
  const size_t e = r.find('}', k);
  if (e == std::string::npos) return;
  float v;
  bool dirty = false;
  if (json_num(r, k, e, "en", &v) && (v != 0) != g_en.load()) { g_en.store(v != 0); dirty = true; }
  if (json_num(r, k, e, "hr", &v) && (v != 0) != g_hr.load()) { g_hr.store(v != 0); dirty = true; }
  if (json_num(r, k, e, "san", &v) && (v != 0) != g_san.load()) { g_san.store(v != 0); dirty = true; }
  if (json_num(r, k, e, "fall", &v) && v >= 0.5f && v <= 100.0f && static_cast<int>(v * 10) != g_fall_x10.load()) {
    g_fall_x10.store(static_cast<int>(v * 10)); dirty = true;
  }
  if (json_num(r, k, e, "ping", &v)) {
    int hz = static_cast<int>(v);
    hz = hz < 0 ? 0 : hz > CONFIG_HIDECARE_VITALS_PING_MAX_HZ ? CONFIG_HIDECARE_VITALS_PING_MAX_HZ : hz;
    g_ping_hz.store(hz);
  }
  if (dirty) { g_cfg_dirty.store(true); ESP_LOGI(TAG, "config en=%d hr=%d san=%d fall=%.1f ping=%d", g_en.load(), g_hr.load(), g_san.load(), g_fall_x10.load() / 10.0, g_ping_hz.load()); }
}
}  // namespace hidecare::vitals
