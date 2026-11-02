/*
 * Hidecare monitor-server client.
 *
 * Registers the device on the Hidecare server, forwards motion state changes
 * and periodic heartbeats to POST /events. OTA goes through the built-in
 * HttpsOtaService, which app_main points at <server>/firmware/latest. All network work runs in its own
 * FreeRTOS task; notify_motion() is safe to call from the frontend loop.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once

#include <string>

namespace hidecare {

/** "HC-" + last 6 hex digits of the station MAC, or the Kconfig override. */
std::string device_code();

// Device code without the "HC-" prefix (used in the setup AP/BLE name).
std::string device_suffix();

// True after the last request to the Hidecare server succeeded.
bool server_ok();

/** Start the background task. Call once after Wi-Fi is started. */
bool start();

/** Queue a motion (true) or no_motion (false) event. Never blocks. */
void notify_motion(bool motion);

}  // namespace hidecare
