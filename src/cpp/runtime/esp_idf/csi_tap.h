/*
 * Hidecare: non-consuming CSI tap.
 *
 * The tap sees every normalized CSI packet (64 centered HT20 subcarriers of
 * interleaved I/Q) in the Wi-Fi capture context, before the detector queue,
 * and never changes what the detector receives. It must be bounded,
 * non-blocking and allocation-free.
 *
 * SPDX-License-Identifier: GPL-3.0-only
 */
#pragma once
#include <cstdint>

namespace espectre {
using csi_tap_fn = void (*)(const int8_t *iq, uint16_t len, int8_t rssi_dbm, int8_t noise_floor_dbm, uint8_t channel);
/** Install (or clear with nullptr) the process-wide tap. */
void set_csi_tap(csi_tap_fn fn);
}  // namespace espectre
