// Hidecare status LED (WS2812 on CONFIG_HIDECARE_LED_GPIO).
#pragma once

namespace hidecare::led {

enum class Mode { OFF, CONNECTED, SETUP, CONNECTING, ERROR };

void start();
void set(Mode mode);

}  // namespace hidecare::led
