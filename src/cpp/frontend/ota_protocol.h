/*
 * ESPectre - Frontend OTA Protocol
 *
 * Author: Francesco Pace <francesco.pace@gmail.com>
 * SPDX-License-Identifier: GPL-3.0-only
 * Commercial licensing available under separate agreement; see LICENSING.md.
 */
#pragma once
#include "espectre_services_sdk.h"


namespace espectre {

/** Official tagged GitHub Release OTA channel. */
inline constexpr const char *ESPECTRE_OTA_CHANNEL_RELEASE = "release";
/** Rolling `main` OTA channel. Fetches GitHub Releases tag `snapshot`. */
inline constexpr const char *ESPECTRE_OTA_CHANNEL_PREVIEW = "preview";
/** Rolling `develop` OTA channel. Fetches GitHub Releases tag `snapshot-dev`. */
inline constexpr const char *ESPECTRE_OTA_CHANNEL_DEVELOP = "develop";
/** GitHub Releases tag for the `preview` OTA channel. Distinct from branch `main`. */
inline constexpr const char *ESPECTRE_OTA_RELEASE_TAG_PREVIEW = "snapshot";
/** GitHub Releases tag for the `develop` OTA channel. Distinct from branch `develop`. */
inline constexpr const char *ESPECTRE_OTA_RELEASE_TAG_DEVELOP = "snapshot-dev";
/**
 * OTA progress, as reported to clients.
 *
 * A check runs `IDLE` -> `CHECKING` -> `UPDATE_AVAILABLE` or `UP_TO_DATE`.
 * An update continues `DOWNLOADING` -> `APPLYING` -> `REBOOT_SCHEDULED`.
 * `ERROR` is terminal for the attempt and carries the reason in
 * `EspectreOtaStatus::message`.
 */
enum class EspectreOtaState : uint8_t {
  IDLE = 0,
  CHECKING,
  UPDATE_AVAILABLE,
  UP_TO_DATE,
  DOWNLOADING,
  APPLYING,
  REBOOT_SCHEDULED,
  ERROR,
};

/** Full OTA status: state, the versions involved, and the resolved URLs. */
struct EspectreOtaStatus {
  EspectreOtaState state{EspectreOtaState::IDLE};
  std::string current_version{"unknown"};
  std::string target_version;
  std::string manifest_url;
  std::string image_url;
  std::string message;
  /** Build-time OTA channel used when a command omits its channel. */
  std::string default_channel;
  /** Resolved OTA channel for the current attempt. Empty when unused. */
  std::string channel;
  bool busy{false};
  bool update_available{false};
};

/** OTA progress payload, for each `IOtaService` status callback worth publishing. */
std::string espectre_ota_status_payload(const EspectreDeviceConfig &config,
                                    const EspectreOtaStatus &status,
                                    uint32_t timestamp_ms);

/**
 * Whether `channel` is a published OTA channel name.
 *
 * Accepted values are `release`, `preview`, and `develop`. Empty is not
 * accepted here; omit the field to keep the firmware default.
 */
bool espectre_ota_channel_accepted(const std::string &channel);
/**
 * Hidecare firmware endpoint: `CONFIG_HIDECARE_SERVER_URL` + `/firmware/latest`.
 *
 * The OTA service appends `?device_code=..&version=..`. The server answers
 * 204 (nothing newer) or `{"version","url","sha256"}`. Upstream GitHub
 * releases are never used. The channel is validated but not sent.
 *
 * @return Empty when the inputs are invalid or Hidecare is not configured.
 */
std::string espectre_ota_manifest_url(const char *frontend, const char *chip, const std::string &channel);

class IOtaService;

/** Shared OTA routes, parameter validation, and event names for Direct and MQTT. */
const EspectreProtocolExtension &frontend_ota_protocol();
/** Execute a successfully parsed OTA command using the application's service and firmware identity. */
FrontendCommandResult execute_frontend_ota_command(
    const EspectreCommand &command, FrontendCommandOrigin origin, IOtaService *service,
    const std::string &current_version, FrontendReadPayloadCallback read_payload);

}  // namespace espectre
