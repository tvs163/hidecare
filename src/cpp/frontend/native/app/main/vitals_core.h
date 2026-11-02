/*
 * Hidecare vitals DSP core (portable C, no ESP-IDF deps; host-testable).
 * Phase pipeline adapted from RuView esp32-csi-node (MIT), see THIRD_PARTY_NOTICES.md.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

#define VC_MAX_SC 64
#define VC_TOP_K 8
#define VC_HIST 256

typedef struct { float b0, b1, b2, a1, a2, x1, x2, y1, y2; } vc_biquad_t;
typedef struct { double mean, m2; uint32_t count; } vc_welford_t;

typedef struct {
  bool phase_sanitize;   /* remove linear phase slope across subcarriers (CFO/STO) */
  bool hr_enabled;
  float fall_thresh;     /* rad/frame^2 on primary phase */
} vc_config_t;

typedef struct {
  float br_bpm, hr_bpm, br_conf, hr_conf, fs_hz;
  bool fall;             /* latched until vc_take_fall() */
  uint32_t frames, rejected;
} vc_output_t;

typedef struct {
  vc_config_t cfg;
  vc_welford_t var[VC_MAX_SC];
  float prev_phase[VC_MAX_SC];
  bool phase_init;
  uint8_t top_k[VC_TOP_K], top_k_n;
  float hist[VC_HIST], br_f[VC_HIST], hr_f[VC_HIST];
  uint16_t hist_len, hist_idx;
  vc_biquad_t bq_br, bq_hr;
  float fs, fs_design;   /* fs = measured frame rate; filters run at fs/dec */
  uint8_t dec, dec_n; float dec_sum;
  int64_t rate_win_start_us; uint32_t rate_win_n;
  float prev_vel, prev_p; bool fall_primed; uint8_t fall_consec; int64_t fall_last_us;
  float hr_ring[13]; uint8_t hr_n, hr_i;
  vc_output_t out;
} vc_state_t;

void vc_init(vc_state_t *s, const vc_config_t *cfg, float fs_initial);
/* Returns false if the frame was rejected by sanitize. iq: interleaved I,Q int8, n_sc subcarriers. */
bool vc_process(vc_state_t *s, const int8_t *iq, uint16_t n_sc, int64_t t_us);
/* Run BPM estimators (~1 Hz). */
void vc_estimate(vc_state_t *s);
/* Utilities exposed for tests. */
bool vc_sanitize(const int8_t *iq, uint16_t n_sc, uint8_t *valid_mask);
void vc_biquad_bandpass(vc_biquad_t *bq, float fs, float lo, float hi);
float vc_biquad_step(vc_biquad_t *bq, float x);
float vc_unwrap(float prev, float curr);
float vc_bpm_zero_crossing(const float *x, uint16_t n, float fs);
float vc_autocorr_bpm(const float *x, uint16_t n, float fs, float lo, float hi, float reject_hz, float *conf);

#ifdef __cplusplus
}
#endif
