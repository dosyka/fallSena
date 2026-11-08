/*
 * rgb_led.h - Analog (PWM) RGB LED driver for the FallSena wearable.
 *
 * Drives a plain 3-pin analog RGB LED (one GPIO per color channel, NO
 * addressable/WS2812-style controller) using the ESP-IDF LEDC peripheral
 * (driver/ledc.h). One LEDC channel per color shares a single LEDC timer.
 *
 * Wiring polarity (common-cathode vs common-anode) is selectable at compile
 * time via RGB_LED_COMMON_ANODE in rgb_led.c, because the physical wiring is
 * not confirmed yet. Default is common-cathode (duty == brightness).
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Convenience named status colors. */
typedef enum {
    RGB_LED_OFF = 0,
    RGB_LED_RED,
    RGB_LED_GREEN,
    RGB_LED_BLUE,
    RGB_LED_YELLOW,
    RGB_LED_CYAN,
    RGB_LED_MAGENTA,
    RGB_LED_WHITE,
} rgb_led_color_t;

/*
 * Configure the LEDC timer and the three LEDC channels (R/G/B) on the GPIOs
 * defined in pins.h. The LED starts off. Returns ESP_OK on success.
 */
esp_err_t rgb_led_init(void);

/*
 * Set the LED to an arbitrary 24-bit color. Each component is 0..255 and is
 * mapped onto the LEDC duty range (polarity-corrected for the wiring type).
 */
esp_err_t rgb_led_set(uint8_t r, uint8_t g, uint8_t b);

/* Set the LED to one of the named status colors. */
esp_err_t rgb_led_set_color(rgb_led_color_t color);

/* Turn all channels off (shortcut for rgb_led_set(0, 0, 0)). */
esp_err_t rgb_led_off(void);

#ifdef __cplusplus
}
#endif
