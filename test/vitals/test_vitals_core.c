/* Host tests for vitals_core (sanitize, filters, estimators, end-to-end synthetic CSI). */
#include "vitals_core.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
static int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void test_sanitize(void) {
  int8_t iq[128] = {0}; uint8_t v[64];
  CHECK(!vc_sanitize(iq, 64, v), "all-zero frame must be rejected");
  for (int k = 0; k < 64; k++) { iq[2*k] = 20; iq[2*k+1] = 5; }
  iq[0] = iq[1] = 0;
  CHECK(vc_sanitize(iq, 64, v) && !v[0] && v[1], "null bin masked");
  for (int k = 0; k < 20; k++) iq[2*k] = 127;
  CHECK(!vc_sanitize(iq, 64, v), "saturated frame rejected");
}
static void test_unwrap(void) {
  float a = vc_unwrap(3.0f, -3.0f);
  CHECK(fabs(a - (2*M_PI - 3.0f)) < 1e-4, "unwrap %f", a);
  CHECK(fabs(vc_unwrap(10.0f, 0.0f) - 4*M_PI) < 1e-3, "multi-turn");
}
static void test_bandpass(void) {
  float fs = 10; vc_biquad_t bq;
  double pin = 0, pout = 0, sin_ = 0, sout = 0;
  vc_biquad_bandpass(&bq, fs, 0.1f, 0.5f);
  for (int i = 0; i < 2000; i++) { float y = vc_biquad_step(&bq, sinf(2*M_PI*0.25f*i/fs)); if (i > 500) pout += y*y, pin += 0.5; }
  vc_biquad_bandpass(&bq, fs, 0.1f, 0.5f);
  for (int i = 0; i < 2000; i++) { float y = vc_biquad_step(&bq, sinf(2*M_PI*2.0f*i/fs)); if (i > 500) sout += y*y, sin_ += 0.5; }
  CHECK(pout/pin > 0.5, "passband gain %f", pout/pin);
  CHECK(sout/sin_ < 0.1, "stopband gain %f", sout/sin_);
}
static void test_estimators(void) {
  float x[256], fs = 10, c;
  for (int i = 0; i < 256; i++) x[i] = sinf(2*M_PI*0.25f*i/fs);
  float z = vc_bpm_zero_crossing(x, 256, fs);
  CHECK(fabsf(z - 15) < 1, "zc 15bpm got %f", z);
  float a = vc_autocorr_bpm(x, 256, fs, 6, 40, 0, &c);
  CHECK(fabsf(a - 15) < 1 && c > 0.5, "ac 15bpm got %f c=%f", a, c);
  for (int i = 0; i < 256; i++) x[i] = sinf(2*M_PI*1.2f*i/fs) + 0.8f*sinf(2*M_PI*0.75f*i/fs);
  float h = vc_autocorr_bpm(x, 256, fs, 45, 180, 0.25f, &c);
  CHECK(fabsf(h - 72) < 6, "hr 72bpm rejecting br harmonic 0.75Hz got %f", h);
  for (int i = 0; i < 256; i++) x[i] = 0;
  CHECK(vc_autocorr_bpm(x, 256, fs, 6, 40, 0, &c) == 0 && c == 0, "silence -> 0");
}
/* synthetic CSI: 64 bins, per-packet random CFO slope, breathing phase on bin 20 */
static void test_end_to_end(void) {
  static vc_state_t s; vc_config_t cfg = { true, true, 15.0f };
  vc_init(&s, &cfg, 50.0f);
  srand(1);
  float fs = 50, br_hz = 0.3f, hr_hz = 1.2f;
  for (int n = 0; n < 50 * 40; n++) {
    int8_t iq[128];
    float slope = ((rand() % 1000) / 1000.0f - 0.5f) * 0.4f, off = (rand() % 628) / 100.0f;
    float t = n / fs;
    for (int k = 0; k < 64; k++) {
      float ph = off + slope * k;
      if (k >= 18 && k <= 22) ph += 0.6f * sinf(2*M_PI*br_hz*t) + 0.08f*sinf(2*M_PI*hr_hz*t);
      float amp = 40 + ((rand() % 5) - 2);
      iq[2*k] = (int8_t)lrintf(amp * cosf(ph)); iq[2*k+1] = (int8_t)lrintf(amp * sinf(ph));
    }
    vc_process(&s, iq, 64, (int64_t)(t * 1e6) + 1);
    if (n % 50 == 0) vc_estimate(&s);
  }
  vc_estimate(&s);
  printf("e2e: fs_eff=%.2f br=%.1f (conf %.2f) hr=%.1f (conf %.2f) fall=%d rej=%u\n", s.out.fs_hz, s.out.br_bpm, s.out.br_conf, s.out.hr_bpm, s.out.hr_conf, s.out.fall, s.out.rejected);
  CHECK(fabsf(s.out.br_bpm - 18) < 2.5, "br 18bpm got %f", s.out.br_bpm);
  CHECK(!s.out.fall, "no fall on breathing");
}
static void test_fall(void) {
  static vc_state_t s; vc_config_t cfg = { false, false, 1.0f };
  vc_init(&s, &cfg, 20);
  int8_t iq[128];
  for (int n = 0; n < 200; n++) {
    float ph = (n > 100 && n < 110) ? ((n % 2) ? 1.2f : -1.2f) : 0.1f + 0.01f * (n % 3);
    for (int k = 0; k < 64; k++) { iq[2*k] = (int8_t)lrintf(40*cosf(ph)); iq[2*k+1] = (int8_t)lrintf(40*sinf(ph)); }
    vc_process(&s, iq, 64, (int64_t)n * 50000 + 1);
    if (n == 100) CHECK(!s.out.fall, "no fall before jerk");
  }
  CHECK(s.out.fall, "fall on phase jerk");
}
int main(void) {
  test_sanitize(); test_unwrap(); test_bandpass(); test_estimators(); test_end_to_end(); test_fall();
  printf(fails ? "%d FAILED\n" : "all vitals_core tests passed\n", fails);
  return fails != 0;
}
