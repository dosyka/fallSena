/*
 * battery.h - LiPo battery voltage / state-of-charge monitor for FallSena.
 *
 * Reads the 3.7V / 1000mAh LiPo battery voltage through the on-board resistor
 * voltage divider on the ESP32-C6 ADC (GPIO3 = ADC1 channel 3), then converts
 * it to a battery percentage.
 *
 * The divider scales the battery voltage DOWN to fit the ADC input range, so
 * the measured pin voltage is multiplied back up by BATTERY_DIVIDER_RATIO (see
 * battery.c) to recover the true battery-terminal voltage.
 *
 * Uses the ESP-IDF ADC oneshot driver (esp_adc/adc_oneshot.h) with curve-
 * fitting calibration when available, falling back to a raw->mV estimate.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Configure the ADC unit/channel for battery sensing (GPIO3 -> ADC1 ch3 on the
 * ESP32-C6), 12 dB attenuation, and set up calibration if supported. Safe to
 * call once at startup. Returns ESP_OK on success.
 */
esp_err_t battery_init(void);

/*
 * Read the battery-terminal voltage in millivolts (i.e. the ADC pin voltage
 * already multiplied back up by the divider ratio). Averages several ADC
 * samples to reduce noise. Returns ESP_OK and writes *out_mv on success.
 */
esp_err_t battery_read_mv(int *out_mv);

/*
 * Read the battery state-of-charge as an integer percentage, clamped 0..100,
 * derived from battery_read_mv() via an approximate LiPo voltage->% curve.
 * Returns ESP_OK and writes *out_pct on success.
 */
esp_err_t battery_read_percent(int *out_pct);

#ifdef __cplusplus
}
#endif
