/*
 * Hidecare vitals DSP core. Adapted from RuView esp32-csi-node edge_processing.c (MIT).
 */
#include "vitals_core.h"
#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define FS_MIN 5.0f
#define FS_MAX 120.0f
#define FALL_CONSEC 3
#define FALL_COOLDOWN_US 5000000LL

/* ---- filters ---- */
void vc_biquad_bandpass(vc_biquad_t *bq, float fs, float lo, float hi) {
  float w0 = 2.0f * (float)M_PI * (lo + hi) / 2.0f / fs;
  float bw = 2.0f * (float)M_PI * (hi - lo) / fs;
  float alpha = sinf(w0) * sinhf(logf(2.0f) / 2.0f * bw / sinf(w0));
  float a0i = 1.0f / (1.0f + alpha);
  bq->b0 = alpha * a0i; bq->b1 = 0.0f; bq->b2 = -alpha * a0i;
  bq->a1 = -2.0f * cosf(w0) * a0i; bq->a2 = (1.0f - alpha) * a0i;
  bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

float vc_biquad_step(vc_biquad_t *bq, float x) {
  float y = bq->b0 * x + bq->b1 * bq->x1 + bq->b2 * bq->x2 - bq->a1 * bq->y1 - bq->a2 * bq->y2;
  bq->x2 = bq->x1; bq->x1 = x; bq->y2 = bq->y1; bq->y1 = y;
  return y;
}

float vc_unwrap(float prev, float curr) {
  float d = curr - prev;
  /* reduce to (-pi, pi] relative to prev */
  d = d - 2.0f * (float)M_PI * floorf((d + (float)M_PI) / (2.0f * (float)M_PI));
  return prev + d;
}

static void welford_update(vc_welford_t *w, double x) {
  w->count++;
  double d = x - w->mean;
  w->mean += d / (double)w->count;
  w->m2 += d * (x - w->mean);
}
static double welford_var(const vc_welford_t *w) { return w->count > 1 ? w->m2 / (double)(w->count - 1) : 0.0; }

/* ---- sanitize: reject empty/saturated frames, mark null subcarriers ---- */
bool vc_sanitize(const int8_t *iq, uint16_t n_sc, uint8_t *valid) {
  uint16_t nz = 0, sat = 0;
  for (uint16_t k = 0; k < n_sc; k++) {
    int8_t i = iq[2 * k], q = iq[2 * k + 1];
    bool ok = (i != 0 || q != 0);
    if (i == -128 || q == -128 || i == 127 || q == 127) { sat++; ok = false; }
    valid[k] = ok;
    nz += ok;
  }
  /* need at least half usable subcarriers and little clipping */
  return nz >= n_sc / 2 && sat <= n_sc / 8;
}

/* ---- BPM estimators ---- */
float vc_bpm_zero_crossing(const float *x, uint16_t n, float fs) {
  if (n < 4 || fs <= 0) return 0.0f;
  uint16_t first = 0, last = 0, cnt = 0;
  for (uint16_t i = 1; i < n; i++)
    if (x[i - 1] <= 0.0f && x[i] > 0.0f) { if (!cnt) first = i; last = i; cnt++; }
  if (cnt < 2) return 0.0f;
  float period = (float)(last - first) / (float)(cnt - 1);
  return period < 1.0f ? 0.0f : fs / period * 60.0f;
}

float vc_autocorr_bpm(const float *x, uint16_t n, float fs, float lo, float hi, float rej_hz, float *conf) {
  if (conf) *conf = 0.0f;
  if (n < 32 || fs <= 0 || hi <= lo) return 0.0f;
  int lmin = (int)(fs * 60.0f / hi), lmax = (int)(fs * 60.0f / lo);
  if (lmin < 2) lmin = 2;
  if (lmax >= (int)n) lmax = (int)n - 1;
  if (lmax <= lmin + 1) return 0.0f;
  float r0 = 0;
  for (uint16_t i = 0; i < n; i++) r0 += x[i] * x[i];
  if (r0 <= 1e-9f) return 0.0f;
  float best = -1; int bl = 0;
  for (int lag = lmin; lag <= lmax; lag++) {
    float f = fs / (float)lag;
    if (rej_hz > 0) {
      int h = 0;
      for (int k = 1; k <= 6 && !h; k++) h = fabsf(f - k * rej_hz) < 0.08f * k * rej_hz;
      if (h) continue;
    }
    float acc = 0;
    for (int i = 0; i + lag < (int)n; i++) acc += x[i] * x[i + lag];
    if (acc > best) { best = acc; bl = lag; }
  }
  if (bl == 0) return 0.0f;
  float c = best / r0;
  if (c < 0.2f) return 0.0f;
  float lr = (float)bl;
  if (bl > lmin && bl < lmax) {
    float a = 0, b = 0;
    for (int i = 0; i + bl + 1 < (int)n; i++) { a += x[i] * x[i + bl - 1]; b += x[i] * x[i + bl + 1]; }
    float den = a - 2 * best + b;
    if (fabsf(den) > 1e-9f) { float d = 0.5f * (a - b) / den; if (d > -1 && d < 1) lr += d; }
  }
  if (conf) *conf = c > 1.0f ? 1.0f : c;
  return fs / lr * 60.0f;
}

static float hr_smooth(vc_state_t *s, float hr) {
  s->hr_ring[s->hr_i] = hr; s->hr_i = (uint8_t)((s->hr_i + 1) % 13);
  if (s->hr_n < 13) s->hr_n++;
  float t[13];
  for (uint8_t i = 0; i < s->hr_n; i++) t[i] = s->hr_ring[i];
  for (uint8_t i = 1; i < s->hr_n; i++) { float v = t[i]; int j = i - 1; while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; } t[j + 1] = v; }
  return t[s->hr_n / 2];
}

/* Decimate to ~10 Hz so the 256-sample history spans ~25 s (enough for breathing). */
static float fs_eff(const vc_state_t *s) { return s->fs / (float)s->dec; }
static void design(vc_state_t *s) {
  int d = (int)(s->fs / 10.0f + 0.5f);
  s->dec = (uint8_t)(d < 1 ? 1 : d > 12 ? 12 : d);
  s->dec_n = 0; s->dec_sum = 0;
  vc_biquad_bandpass(&s->bq_br, fs_eff(s), 0.1f, 0.5f);
  vc_biquad_bandpass(&s->bq_hr, fs_eff(s), 0.8f, 2.0f);
  s->fs_design = s->fs;
}

void vc_init(vc_state_t *s, const vc_config_t *cfg, float fs) {
  memset(s, 0, sizeof(*s));
  s->cfg = *cfg;
  s->fs = fs < FS_MIN ? FS_MIN : fs;
  design(s);
}

/* Scratch is static: vc_process runs in a single task only. */
static float s_br_lin[VC_HIST], s_hr_lin[VC_HIST];

bool vc_process(vc_state_t *s, const int8_t *iq, uint16_t n_sc, int64_t t_us) {
  if (n_sc > VC_MAX_SC) n_sc = VC_MAX_SC;
  uint8_t valid[VC_MAX_SC];
  if (n_sc < 8 || !vc_sanitize(iq, n_sc, valid)) { s->out.rejected++; return false; }
  s->out.frames++;

  /* adaptive sample rate: count frames per >=1 s window, EMA */
  if (s->rate_win_start_us == 0 || t_us <= s->rate_win_start_us) { s->rate_win_start_us = t_us; s->rate_win_n = 0; }
  else {
    s->rate_win_n++;
    int64_t el = t_us - s->rate_win_start_us;
    if (el >= 1000000) {
      float m = (float)s->rate_win_n * 1e6f / (float)el;
      m = m < FS_MIN ? FS_MIN : (m > FS_MAX ? FS_MAX : m);
      s->fs = s->fs + 0.25f * (m - s->fs);
      s->rate_win_start_us = t_us; s->rate_win_n = 0;
    }
  }
  if (fabsf(s->fs - s->fs_design) > 0.15f * s->fs_design) design(s);

  /* phase extraction (+ optional linear sanitize across subcarriers) */
  float ph[VC_MAX_SC];
  for (uint16_t k = 0; k < n_sc; k++) ph[k] = atan2f((float)iq[2 * k + 1], (float)iq[2 * k]);
  if (s->cfg.phase_sanitize) {
    /* unwrap along frequency, then subtract least-squares line over valid bins */
    for (uint16_t k = 1; k < n_sc; k++) ph[k] = vc_unwrap(ph[k - 1], ph[k]);
    float sx = 0, sy = 0, sxx = 0, sxy = 0; int m = 0;
    for (uint16_t k = 0; k < n_sc; k++) if (valid[k]) { sx += k; sy += ph[k]; sxx += (float)k * k; sxy += k * ph[k]; m++; }
    float den = m * sxx - sx * sx;
    if (m >= 2 && den != 0) {
      float a = (m * sxy - sx * sy) / den, b = (sy - a * sx) / m;
      for (uint16_t k = 0; k < n_sc; k++) ph[k] -= a * k + b;
    }
  }
  /* unwrap in time + Welford variance */
  for (uint16_t k = 0; k < n_sc; k++) {
    if (s->phase_init) ph[k] = vc_unwrap(s->prev_phase[k], ph[k]);
    s->prev_phase[k] = ph[k];
    if (valid[k]) welford_update(&s->var[k], ph[k]);
  }
  s->phase_init = true;

  /* top-K by variance, refreshed every 100 frames */
  if (s->top_k_n == 0 || (s->out.frames % 100) == 1) {
    uint8_t used[VC_MAX_SC] = {0};
    s->top_k_n = 0;
    for (uint8_t i = 0; i < VC_TOP_K; i++) {
      double bv = -1; int bi = -1;
      for (uint16_t k = 0; k < n_sc; k++) if (!used[k] && s->var[k].count > 1 && welford_var(&s->var[k]) > bv) { bv = welford_var(&s->var[k]); bi = k; }
      if (bi < 0) break;
      used[bi] = 1; s->top_k[s->top_k_n++] = (uint8_t)bi;
    }
  }
  if (s->top_k_n == 0) return true;

  float p = ph[s->top_k[0]];
  /* fall: phase acceleration on the raw frame stream, consecutive + cooldown */
  if (s->fall_primed) {
    float v = p - s->prev_p;
    float acc = fabsf(v - s->prev_vel);
    s->prev_vel = v;
    s->fall_consec = acc > s->cfg.fall_thresh ? (uint8_t)(s->fall_consec + 1) : 0;
    if (s->fall_consec >= FALL_CONSEC && t_us - s->fall_last_us >= FALL_COOLDOWN_US) {
      s->out.fall = true; s->fall_last_us = t_us; s->fall_consec = 0;
    }
  }
  s->fall_primed = true; s->prev_p = p;

  s->dec_sum += p;
  if (++s->dec_n < s->dec) return true;
  p = s->dec_sum / (float)s->dec_n;
  s->dec_n = 0; s->dec_sum = 0;
  s->hist[s->hist_idx] = p;
  s->br_f[s->hist_idx] = vc_biquad_step(&s->bq_br, p);
  s->hr_f[s->hist_idx] = s->cfg.hr_enabled ? vc_biquad_step(&s->bq_hr, p) : 0.0f;
  s->hist_idx = (uint16_t)((s->hist_idx + 1) % VC_HIST);
  if (s->hist_len < VC_HIST) s->hist_len++;
  s->out.fs_hz = fs_eff(s);
  return true;
}

/* Heavier estimators: called at low rate (e.g. 1 Hz), not per frame. */
void vc_estimate(vc_state_t *s) {
  if (s->hist_len < 64) return;
  uint16_t n = s->hist_len;
  for (uint16_t i = 0; i < n; i++) {
    uint16_t r = (uint16_t)((s->hist_idx + VC_HIST - n + i) % VC_HIST);
    s_br_lin[i] = s->br_f[r]; s_hr_lin[i] = s->hr_f[r];
  }
  float bc = 0, hc = 0;
  float br = vc_bpm_zero_crossing(s_br_lin, n, fs_eff(s));
  float br_rob = vc_autocorr_bpm(s_br_lin, n, fs_eff(s), 6.0f, 40.0f, 0.0f, &bc);
  if (br >= 6.0f && br <= 40.0f && bc > 0) { s->out.br_bpm = br; s->out.br_conf = bc; } else s->out.br_conf = 0;
  if (s->cfg.hr_enabled) {
    float hr = vc_autocorr_bpm(s_hr_lin, n, fs_eff(s), 45.0f, 180.0f, br_rob / 60.0f, &hc);
    if (hr >= 40.0f && hr <= 180.0f) { s->out.hr_bpm = hr_smooth(s, hr); s->out.hr_conf = hc; } else s->out.hr_conf = 0;
  }
}
