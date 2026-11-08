/*
 * vibration.h - Coin vibration motor driver for the FallSena wearable.
 *
 * Simple on/off control of a coin vibration motor via a single GPIO. The GPIO
 * is expected to drive the motor through a transistor/MOSFET (never directly).
 * Active-high: GPIO high == motor on.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Configure the motor GPIO as an output, driven low (motor off). */
esp_err_t vibration_init(void);

/* Turn the motor on / off. */
esp_err_t vibration_on(void);
esp_err_t vibration_off(void);

/*
 * Vibrate for `ms` milliseconds, then stop.
 *
 * NOTE: this BLOCKS the calling task for `ms` (implemented with vTaskDelay),
 * so do not call it from a time-critical loop with large values.
 */
esp_err_t vibration_pulse(uint32_t ms);

#ifdef __cplusplus
}
#endif
