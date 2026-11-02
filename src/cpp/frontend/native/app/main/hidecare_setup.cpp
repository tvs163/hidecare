/*
 * Hidecare setup mode: open AP "Hidecare-XXXXXX", catch-all DNS, captive
 * portal on port 80, and Improv BLE. See hidecare_setup.h.
 */
#include "hidecare_setup.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <vector>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "sdkconfig.h"

#include "hidecare_client.h"
#include "hidecare_portal_page.h"

namespace hidecare::setup {
namespace {

const char *TAG = "hidecare.setup";

constexpr uint32_t kBootGraceMs = 60U * 1000U;            // first connect at boot
constexpr uint32_t kLostGraceMs = 10U * 60U * 1000U;      // after a working link drops
constexpr uint32_t kStopAfterSuccessMs = 30U * 1000U;
constexpr uint32_t kCodeLogPeriodMs = 30U * 1000U;
constexpr uint32_t kConnectTimeoutMs = 75U * 1000U;
constexpr uint32_t kScanTimeoutMs = 15U * 1000U;
constexpr uint32_t kScanCacheMs = 20U * 1000U;

Hooks g_hooks;
std::mutex g_mtx;  // guards everything below that HTTP/DNS/BLE tasks touch

// Requests from other tasks.
bool g_pending_connect = false;
std::string g_pending_ssid, g_pending_pass;
bool g_scan_requested = false;

// Snapshot for other tasks.
ConnectResult g_result = ConnectResult::PENDING;
bool g_result_valid = false;
std::string g_result_msg;
std::string g_scan_json = "[]";
bool g_scanning = false;
std::string g_ip;

std::atomic<bool> g_active{false};
std::atomic<bool> g_scan_done{false};
std::atomic<bool> g_dns_run{false};
TaskHandle_t g_dns_task = nullptr;
httpd_handle_t g_httpd = nullptr;
esp_netif_t *g_ap_netif = nullptr;
bool g_inited = false;

// Loop-task-only state.
bool g_ever_connected = false;
uint32_t g_last_connected_ms = 0;
uint32_t g_stop_at_ms = 0;
uint32_t g_next_code_log_ms = 0;
uint32_t g_next_mode_check_ms = 0;
bool g_connecting = false;
uint32_t g_connect_started_ms = 0;
uint32_t g_scan_started_ms = 0;
uint32_t g_last_scan_ms = 0;
bool g_scan_running = false;

uint32_t now_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

std::string code() { return hidecare::device_code(); }

std::string ap_ssid() {
  std::string c = code();
  if (c.rfind("HC-", 0) == 0) c = c.substr(3);
  return "Hidecare-" + c;
}

std::string json_escape(const std::string &s) {
  std::string out;
  for (unsigned char ch : s) {
    if (ch == '"' || ch == '\\') {
      out += '\\';
      out += static_cast<char>(ch);
    } else if (ch < 0x20) {
      char buf[8];
      snprintf(buf, sizeof(buf), "\\u%04x", ch);
      out += buf;
    } else {
      out += static_cast<char>(ch);
    }
  }
  return out;
}

std::string url_decode(const std::string &s) {
  std::string out;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '+') {
      out += ' ';
    } else if (s[i] == '%' && i + 2 < s.size()) {
      char hex[3] = {s[i + 1], s[i + 2], 0};
      out += static_cast<char>(strtol(hex, nullptr, 16));
      i += 2;
    } else {
      out += s[i];
    }
  }
  return out;
}

// ---------------------------------------------------------------- DNS

void dns_task(void *) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (sock < 0) {
    ESP_LOGE(TAG, "DNS socket failed");
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  if (bind(sock, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
    ESP_LOGE(TAG, "DNS bind failed");
    close(sock);
    g_dns_task = nullptr;
    vTaskDelete(nullptr);
    return;
  }
  timeval tv{1, 0};
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  uint8_t buf[512];
  while (g_dns_run.load()) {
    sockaddr_in from{};
    socklen_t from_len = sizeof(from);
    const int len = recvfrom(sock, buf, sizeof(buf) - 16, 0, reinterpret_cast<sockaddr *>(&from), &from_len);
    if (len < 12) continue;
    const uint16_t qdcount = (buf[4] << 8) | buf[5];
    if ((buf[2] & 0x80) != 0 || qdcount < 1) continue;  // not a query
    // Walk the first question name.
    int pos = 12;
    while (pos < len && buf[pos] != 0) pos += buf[pos] + 1;
    pos += 5;  // zero byte + qtype + qclass
    if (pos > len) continue;
    const uint16_t qtype = (buf[pos - 4] << 8) | buf[pos - 3];
    int out = pos;
    buf[2] = 0x84 | (buf[2] & 0x01);  // response, authoritative, keep RD
    buf[3] = 0x80;                    // RA, no error
    buf[4] = 0;
    buf[5] = 1;  // one question
    buf[8] = buf[9] = buf[10] = buf[11] = 0;
    buf[6] = 0;
    buf[7] = 0;
    if (qtype == 1 /* A */) {
      buf[7] = 1;
      const uint8_t answer[] = {0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 60, 0, 4, 192, 168, 4, 1};
      memcpy(buf + out, answer, sizeof(answer));
      out += sizeof(answer);
    }
    sendto(sock, buf, out, 0, reinterpret_cast<sockaddr *>(&from), from_len);
  }
  close(sock);
  g_dns_task = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------- HTTP

esp_err_t send_json(httpd_req_t *req, const std::string &body) {
  httpd_resp_set_type(req, "application/json; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, body.c_str(), body.size());
}

esp_err_t page_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html; charset=utf-8");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, kPortalPage, sizeof(kPortalPage) - 1);
}

esp_err_t scan_handler(httpd_req_t *req) {
  std::string body;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_scan_requested = true;
    body = "{\"code\":\"" + json_escape(code()) + "\",\"scanning\":" + (g_scanning ? "true" : "false") +
           ",\"networks\":" + g_scan_json + "}";
  }
  return send_json(req, body);
}

esp_err_t status_handler(httpd_req_t *req) {
  std::string state = "idle", msg, ip;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_result_valid) {
      state = g_result == ConnectResult::OK ? "ok" : g_result == ConnectResult::FAILED ? "failed" : "pending";
    }
    msg = g_result_msg;
    ip = g_ip;
  }
  return send_json(req, "{\"state\":\"" + state + "\",\"msg\":\"" + json_escape(msg) + "\",\"code\":\"" +
                            json_escape(code()) + "\",\"url\":\"" + json_escape(bot_url()) + "\",\"ip\":\"" +
                            json_escape(ip) + "\"}");
}

esp_err_t connect_handler(httpd_req_t *req) {
  if (req->content_len <= 0 || req->content_len > 512) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    return ESP_FAIL;
  }
  char buf[513] = {};
  int got = 0;
  while (got < static_cast<int>(req->content_len)) {
    const int r = httpd_req_recv(req, buf + got, req->content_len - got);
    if (r <= 0) {
      if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
      return ESP_FAIL;
    }
    got += r;
  }
  char ssid[100] = {}, pass[200] = {};
  httpd_query_key_value(buf, "ssid", ssid, sizeof(ssid));
  httpd_query_key_value(buf, "password", pass, sizeof(pass));
  const bool ok = queue_connect(url_decode(ssid), url_decode(pass));
  return send_json(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
}

esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  return httpd_resp_send(req, "", 0);
}

bool start_http() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = 80;
  config.ctrl_port = 32790;
  config.max_open_sockets = 3;
  config.max_uri_handlers = 6;
  config.lru_purge_enable = true;
  config.stack_size = 6144;
  if (httpd_start(&g_httpd, &config) != ESP_OK) {
    g_httpd = nullptr;
    return false;
  }
  const httpd_uri_t uris[] = {
      {"/", HTTP_GET, page_handler, nullptr},
      {"/scan", HTTP_GET, scan_handler, nullptr},
      {"/status", HTTP_GET, status_handler, nullptr},
      {"/connect", HTTP_POST, connect_handler, nullptr},
  };
  for (const auto &u : uris) httpd_register_uri_handler(g_httpd, &u);
  httpd_register_err_handler(g_httpd, HTTPD_404_NOT_FOUND, redirect_404);
  return true;
}

// ---------------------------------------------------------------- AP

void scan_done_handler(void *, esp_event_base_t, int32_t, void *) { g_scan_done.store(true); }

bool configure_ap() {
  wifi_config_t ap{};
  const std::string ssid = ap_ssid();
  memcpy(ap.ap.ssid, ssid.c_str(), ssid.size());
  ap.ap.ssid_len = ssid.size();
  ap.ap.channel = 1;
  ap.ap.authmode = WIFI_AUTH_OPEN;
  ap.ap.max_connection = 4;
  ap.ap.beacon_interval = 100;
  return esp_wifi_set_config(WIFI_IF_AP, &ap) == ESP_OK;
}

void start_setup() {
  if (g_ap_netif == nullptr) {
    g_ap_netif = esp_netif_create_default_wifi_ap();
  }
  esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
  if (err != ESP_OK || !configure_ap()) {
    ESP_LOGE(TAG, "Failed to raise the setup AP: %s", esp_err_to_name(err));
  }
  if (g_ap_netif != nullptr) {
    static char uri[] = "http://192.168.4.1/";
    esp_netif_dhcps_stop(g_ap_netif);
    esp_netif_dhcps_option(g_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri));
    esp_netif_dhcps_start(g_ap_netif);
  }
  g_dns_run.store(true);
  if (g_dns_task == nullptr) {
    xTaskCreate(dns_task, "hc_dns", 3072, nullptr, 4, &g_dns_task);
  }
  if (g_httpd == nullptr && !start_http()) {
    ESP_LOGE(TAG, "Failed to start the setup web server");
  }
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_result_valid = false;
    g_result_msg.clear();
    g_scan_requested = true;
  }
  if (!ble::start(ap_ssid())) {
    ESP_LOGW(TAG, "Improv BLE did not start; use the Wi-Fi setup page");
  }
  g_active.store(true);
  g_stop_at_ms = 0;
  g_next_code_log_ms = 0;
  ESP_LOGW(TAG, "Setup mode ON: connect to Wi-Fi \"%s\" and open http://192.168.4.1/ (device code %s)",
           ap_ssid().c_str(), code().c_str());
}

void stop_setup() {
  if (g_httpd != nullptr) {
    httpd_stop(g_httpd);
    g_httpd = nullptr;
  }
  g_dns_run.store(false);
  wifi_mode_t mode;
  if (esp_wifi_get_mode(&mode) == ESP_OK && mode != WIFI_MODE_STA) {
    esp_wifi_set_mode(WIFI_MODE_STA);
  }
  ble::stop();
  g_active.store(false);
  ESP_LOGI(TAG, "Setup mode OFF (Wi-Fi connected), access point and BLE stopped");
}

void finish_scan() {
  g_scan_running = false;
  g_last_scan_ms = now_ms();
  uint16_t count = 0;
  std::vector<wifi_ap_record_t> recs;
  if (esp_wifi_scan_get_ap_num(&count) == ESP_OK && count > 0) {
    count = std::min<uint16_t>(count, 30);
    recs.resize(count);
    if (esp_wifi_scan_get_ap_records(&count, recs.data()) != ESP_OK) count = 0;
    recs.resize(count);
  }
  std::sort(recs.begin(), recs.end(), [](const wifi_ap_record_t &a, const wifi_ap_record_t &b) { return a.rssi > b.rssi; });
  std::vector<std::string> seen;
  std::string json = "[";
  const std::string own = ap_ssid();
  for (const auto &r : recs) {
    std::string s(reinterpret_cast<const char *>(r.ssid));
    if (s.empty() || s == own || std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
    seen.push_back(s);
    const char *q = r.rssi > -60 ? "отлично" : r.rssi > -72 ? "хорошо" : "слабо";
    if (json.size() > 1) json += ",";
    json += "{\"s\":\"" + json_escape(s) + "\",\"l\":" + (r.authmode == WIFI_AUTH_OPEN ? "false" : "true") +
            ",\"q\":\"" + q + "\"}";
    if (seen.size() >= 20) break;
  }
  json += "]";
  std::lock_guard<std::mutex> lock(g_mtx);
  g_scan_json = json;
  g_scanning = false;
}

void handle_scan(uint32_t now) {
  if (g_scan_running) {
    if (g_scan_done.exchange(false)) {
      finish_scan();
    } else if (now - g_scan_started_ms > kScanTimeoutMs) {
      esp_wifi_scan_stop();
      g_scan_running = false;
      std::lock_guard<std::mutex> lock(g_mtx);
      g_scanning = false;
    }
    return;
  }
  bool want = false;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    want = g_scan_requested && (g_last_scan_ms == 0 || now - g_last_scan_ms > kScanCacheMs);
    g_scan_requested = false;
  }
  if (!want || g_connecting) return;
  wifi_scan_config_t cfg{};
  g_scan_done.store(false);
  if (esp_wifi_scan_start(&cfg, false) == ESP_OK) {
    g_scan_running = true;
    g_scan_started_ms = now;
    std::lock_guard<std::mutex> lock(g_mtx);
    g_scanning = true;
  } else {
    std::lock_guard<std::mutex> lock(g_mtx);
    g_scan_requested = true;  // the station is busy connecting; retry soon
  }
}

void set_result(ConnectResult r, const std::string &msg) {
  std::lock_guard<std::mutex> lock(g_mtx);
  g_result = r;
  g_result_valid = true;
  g_result_msg = msg;
}

void handle_connect(uint32_t now, bool connected) {
  bool start = false;
  std::string ssid, pass;
  {
    std::lock_guard<std::mutex> lock(g_mtx);
    if (g_pending_connect) {
      start = true;
      ssid = g_pending_ssid;
      pass = g_pending_pass;
      g_pending_connect = false;
      g_pending_pass.assign(g_pending_pass.size(), '\0');
      g_pending_pass.clear();
    }
  }
  if (start) {
    if (g_scan_running) {
      esp_wifi_scan_stop();
      g_scan_running = false;
    }
    std::string message;
    ESP_LOGI(TAG, "Setup: connecting to \"%s\"", ssid.c_str());
    if (g_hooks.begin_connect && g_hooks.begin_connect(ssid, pass, &message)) {
      g_connecting = true;
      g_connect_started_ms = now;
      set_result(ConnectResult::PENDING, "");
      ble::on_connect_started();
    } else {
      set_result(ConnectResult::FAILED, message.empty() ? "устройство занято, попробуйте ещё раз" : message);
      ble::on_connect_finished(false);
    }
    return;
  }
  if (!g_connecting || now - g_connect_started_ms < 1500) return;
  const ConnectResult r = g_hooks.connect_result ? g_hooks.connect_result() : ConnectResult::PENDING;
  if (r == ConnectResult::OK && connected) {
    g_connecting = false;
    set_result(ConnectResult::OK, "");
    ESP_LOGW(TAG, "Setup: Wi-Fi connected. Device code %s — add it in @hidecarebot", code().c_str());
    ble::on_connect_finished(true);
  } else if (r == ConnectResult::FAILED || now - g_connect_started_ms > kConnectTimeoutMs) {
    g_connecting = false;
    set_result(ConnectResult::FAILED, "сеть не ответила или пароль неверный");
    ESP_LOGW(TAG, "Setup: connection failed");
    ble::on_connect_finished(false);
  }
}

}  // namespace

std::string bot_url() {
  std::string base = CONFIG_HIDECARE_SETUP_URL;
  return base + (base.find('?') == std::string::npos ? "?start=" : "&start=") + code();
}

bool active() { return g_active.load(); }

bool queue_connect(const std::string &ssid, const std::string &password) {
  if (ssid.empty() || ssid.size() > 32 || password.size() > 64) {
    set_result(ConnectResult::FAILED, "неверное имя сети или пароль");
    return false;
  }
  std::lock_guard<std::mutex> lock(g_mtx);
  g_pending_connect = true;
  g_pending_ssid = ssid;
  g_pending_pass = password;
  g_result = ConnectResult::PENDING;
  g_result_valid = true;
  g_result_msg.clear();
  return true;
}

ConnectResult last_result(std::string *message) {
  std::lock_guard<std::mutex> lock(g_mtx);
  if (message != nullptr) *message = g_result_msg;
  return g_result_valid ? g_result : ConnectResult::PENDING;
}

void init(Hooks hooks) {
  g_hooks = std::move(hooks);
  esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, scan_done_handler, nullptr);
  g_inited = true;
  ESP_LOGI(TAG, "Hidecare device code: %s", code().c_str());
}

void loop() {
  if (!g_inited) return;
  const uint32_t now = now_ms();
  const bool connected = g_hooks.connected && g_hooks.connected();
  if (connected) {
    g_ever_connected = true;
    g_last_connected_ms = now;
  }

  if (!g_active.load()) {
    const bool has_saved = g_hooks.has_saved_wifi && g_hooks.has_saved_wifi();
    const bool want = !connected && !g_connecting &&
                      (!has_saved || (!g_ever_connected && now > kBootGraceMs) ||
                       (g_ever_connected && now - g_last_connected_ms > kLostGraceMs));
    if (want) start_setup();
    return;
  }

  handle_connect(now, connected);
  handle_scan(now);

  if (static_cast<int32_t>(now - g_next_code_log_ms) >= 0) {
    g_next_code_log_ms = now + kCodeLogPeriodMs;
    ESP_LOGW(TAG, "SETUP MODE: device code %s, Wi-Fi \"%s\", http://192.168.4.1/", code().c_str(),
             ap_ssid().c_str());
    if (connected && g_hooks.ip_address) {
      std::lock_guard<std::mutex> lock(g_mtx);
      g_ip = g_hooks.ip_address();
    }
  }

  // The ESPectre Wi-Fi lifecycle may rebuild the driver in STA mode; keep
  // the setup AP alive while setup mode is on.
  if (static_cast<int32_t>(now - g_next_mode_check_ms) >= 0) {
    g_next_mode_check_ms = now + 2000U;
    wifi_mode_t mode;
    if (esp_wifi_get_mode(&mode) == ESP_OK && mode == WIFI_MODE_STA) {
      if (esp_wifi_set_mode(WIFI_MODE_APSTA) == ESP_OK) configure_ap();
    }
  }

  if (connected && !g_connecting) {
    if (g_stop_at_ms == 0) g_stop_at_ms = now + kStopAfterSuccessMs;
    if (static_cast<int32_t>(now - g_stop_at_ms) >= 0) stop_setup();
  } else {
    g_stop_at_ms = 0;
  }
}

}  // namespace hidecare::setup
