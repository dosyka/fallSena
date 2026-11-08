/*
 * pins.h - Central pin / peripheral configuration for the FallSena wearable.
 *
 * This header documents ALL planned peripheral GPIOs for the ESP32-C6 board,
 * including peripherals that are not driven yet, so wiring stays in one place.
 *
 * GPIO ASSIGNMENTS CONFIRMED on 2026-06-30.
 * Every GPIO number below was verified against the real FallSena hardware
 * using the self-test firmware (src/firmware_selftest/), which exercised and
 * passed every peripheral on actual hardware: the I2C MPU6050 IMU, the RGB
 * LED, the vibration motor, the push button, and the battery ADC.
 *
 * ESP32-C6 GPIO notes:
 *   - Usable general GPIOs: 0..30 (some are strapping pins: 4, 5, 8, 9, 15).
 *   - GPIO 12/13 are typically USB-Serial-JTAG (D-/D+); avoid for peripherals.
 *   - ADC1 channels are on GPIO 0..6 on the ESP32-C6.
 */

#pragma once

/* ------------------------------------------------------------------ */
/* I2C bus - MPU6050 6-axis IMU (USED)                                 */
/* ------------------------------------------------------------------ */

/* I2C controller port number. ESP32-C6 has one HP I2C port (0). */
#define FALLSENA_I2C_PORT_NUM      0

/* I2C SDA line. Confirmed: GPIO22 (verified on real HW). */
#define FALLSENA_I2C_SDA_GPIO      22

/* I2C SCL line. Confirmed: GPIO23 (verified on real HW). */
#define FALLSENA_I2C_SCL_GPIO      23

/* I2C bus clock. 400 kHz fast-mode is supported by the MPU6050. */
#define FALLSENA_I2C_FREQ_HZ       400000

/* MPU6050 7-bit I2C address. 0x68 when AD0 is low, 0x69 when AD0 is high. */
#define FALLSENA_MPU6050_ADDR      0x68

/* ------------------------------------------------------------------ */
/* RGB LED - analog 3-pin PWM via LEDC (USED, confirmed)               */
/* ------------------------------------------------------------------ */

/* Common-cathode 3-pin RGB LED, one GPIO per channel. Driven by
 * rgb_led.c via LEDC (one PWM channel per color). Polarity
 * (common-cathode vs -anode) is set in rgb_led.c. */
#define FALLSENA_RGB_LED_R_GPIO    0   /* Red channel   (confirmed) */
#define FALLSENA_RGB_LED_G_GPIO    20  /* Green channel (confirmed) */
#define FALLSENA_RGB_LED_B_GPIO    18  /* Blue channel  (confirmed) */

/* ------------------------------------------------------------------ */
/* Coin vibration motor - GPIO on/off (USED, confirmed)               */
/* ------------------------------------------------------------------ */

/* Driven via transistor/MOSFET, never direct from GPIO.
 * Driven by vibration.c (active-high GPIO output). */
#define FALLSENA_VIBRATION_GPIO    21  /* Vibration motor (confirmed) */

/* ------------------------------------------------------------------ */
/* Push button - debounced short/long press (USED, confirmed)         */
/* ------------------------------------------------------------------ */

/* Active-low to GND, internal pull-up enabled by button.c.
 * GPIO1 is a normal (non-strapping) GPIO on the ESP32-C6. */
#define FALLSENA_BUTTON_GPIO       1   /* Push button (confirmed) */

/* ------------------------------------------------------------------ */
/* Battery voltage sensing (USED, confirmed)                          */
/* ------------------------------------------------------------------ */

/* Must be an ADC1-capable pin (GPIO0..6 on ESP32-C6). Driven by
 * battery.c via the ADC oneshot driver. GPIO2 maps to ADC1 channel 2
 * on the ESP32-C6. The battery+ is read through an on-board resistor
 * voltage divider; set BATTERY_DIVIDER_RATIO in battery.c to match the
 * actual resistors. */
#define FALLSENA_BATTERY_ADC_GPIO  2   /* Battery divider tap (confirmed) */
