#include "hidecare_led.h"

#include "sdkconfig.h"

#if CONFIG_HIDECARE_LED_ENABLED

#include <atomic>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "led_strip.h"

namespace hidecare::led {
namespace {

const char *TAG = "hidecare.led";
std::atomic<Mode> g_mode{Mode::CONNECTING};
led_strip_handle_t g_strip = nullptr;

void put(uint8_t r, uint8_t g, uint8_t b) {
  const uint32_t k = CONFIG_HIDECARE_LED_BRIGHTNESS;
  led_strip_set_pixel(g_strip, 0, r * k / 255, g * k / 255, b * k / 255);
  led_strip_refresh(g_strip);
}

void task(void *) {
  uint32_t tick = 0;  // 100 ms
  Mode last = Mode::OFF;
  bool last_on = false;
  while (true) {
    const Mode mode = g_mode.load();
    bool on = true;
    uint8_t r = 0, g = 0, b = 0;
    switch (mode) {
      case Mode::CONNECTED: g = 255; break;                                     // steady green
      case Mode::SETUP: b = 255; on = (tick % 20) < 10; break;                  // 1 s on / 1 s off
      case Mode::CONNECTING: r = 255; g = 160; on = (tick % 10) < 5; break;     // yellow 0.5 s
      case Mode::ERROR: r = 255; on = (tick % 4) < 2; break;                    // red 0.2 s
      case Mode::OFF: on = false; break;
    }
    if (mode != last || on != last_on) {
      if (on) {
        put(r, g, b);
      } else {
        led_strip_clear(g_strip);
      }
      last = mode;
      last_on = on;
    }
    ++tick;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
}

}  // namespace

void start() {
  led_strip_config_t strip{};
  strip.strip_gpio_num = CONFIG_HIDECARE_LED_GPIO;
  strip.max_leds = 1;
  strip.led_model = LED_MODEL_WS2812;
  strip.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
  led_strip_rmt_config_t rmt{};
  rmt.resolution_hz = 10 * 1000 * 1000;
  if (led_strip_new_rmt_device(&strip, &rmt, &g_strip) != ESP_OK) {
    ESP_LOGW(TAG, "No status LED on GPIO%d", CONFIG_HIDECARE_LED_GPIO);
    g_strip = nullptr;
    return;
  }
  led_strip_clear(g_strip);
  xTaskCreate(task, "hc_led", 2560, nullptr, 2, nullptr);
  ESP_LOGI(TAG, "Status LED on GPIO%d", CONFIG_HIDECARE_LED_GPIO);
}

void set(Mode mode) { g_mode.store(mode); }

}  // namespace hidecare::led

#else

namespace hidecare::led {
void start() {}
void set(Mode) {}
}  // namespace hidecare::led

#endif
