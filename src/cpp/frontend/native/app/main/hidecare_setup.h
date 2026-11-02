/*
 * Hidecare setup mode: captive portal (open AP + DNS + HTTP) and Improv Wi-Fi
 * over BLE. Both run while the device has no working Wi-Fi and both feed the
 * same connect request into the ESPectre provisioning service.
 *
 * Threading: loop() and every Hooks callback run on the ESPectre loop task.
 * HTTP, DNS and BLE tasks only queue requests and read a mutex-protected
 * snapshot.
 */
#pragma once

#include <functional>
#include <string>

namespace hidecare::setup {

enum class ConnectResult { PENDING, OK, FAILED };

struct Hooks {
  std::function<bool()> connected;        // STA has an IP
  std::function<bool()> has_saved_wifi;   // an SSID is stored
  std::function<bool(const std::string &, const std::string &, std::string *)> begin_connect;
  std::function<ConnectResult()> connect_result;
  std::function<std::string()> ip_address;
};

// Called once from app_main after Wi-Fi started.
void init(Hooks hooks);
// Called from the loop task.
void loop();
// True while the AP and/or BLE are up.
bool active();
// URL the user should open after a successful setup.
std::string bot_url();

// Used by the BLE/HTTP front ends (any task).
bool queue_connect(const std::string &ssid, const std::string &password);
ConnectResult last_result(std::string *message);

// BLE Improv (hidecare_ble.cpp). No-ops when BLE is disabled in sdkconfig.
namespace ble {
bool start(const std::string &name);
void stop();
bool running();
void on_connect_started();
void on_connect_finished(bool ok);
}  // namespace ble

}  // namespace hidecare::setup
