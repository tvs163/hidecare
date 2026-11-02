// Hidecare vitals (3.5): breathing/heart-rate/phase-fall estimator running next
// to the ESPectre detector. Never changes what the detector sees.
#pragma once
#include <cstdint>
#include <string>

namespace hidecare::vitals {
struct Snapshot {
  bool enabled;
  float br_bpm, hr_bpm, br_conf, hr_conf, fs_hz;
  bool fall_phase;      // a phase-acceleration fall happened since the last snapshot
  uint32_t frames, rejected, dropped;
  uint16_t ping_hz;
};
bool start();
// Wi-Fi capture context: bounded, non-blocking, allocation-free.
void feed(const int8_t *iq, uint16_t len);
// Called once per telemetry window; clears the fall latch.
Snapshot take();
// Server-driven runtime config, from the /telemetry reply: "vitals":{"en":1,"hr":1,"san":1,"fall":15,"ping":50}
void apply_config_json(const std::string &resp);
}  // namespace hidecare::vitals
