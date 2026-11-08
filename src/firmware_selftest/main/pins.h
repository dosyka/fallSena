/*
 * pins.h - Central pin / peripheral configuration for the FallSena wearable.
 *
 * This header documents ALL planned peripheral GPIOs for the ESP32-C6 board,
 * including peripherals that are not driven yet, so wiring stays in one place.
 *
 * !!! OWNER ACTION REQUIRED !!!
 * The GPIO numbers below are sensible, electrically-valid ESP32-C6 defaults
 * chosen for documentation. They have NOT been confirmed against the actual
 * FallSena hardware wiring. Before flashing to real hardware, the OWNER must
 * verify every pin against the schematic / breadboard and correct as needed.
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

/* I2C SDA line. Default: GPIO6 (OWNER: confirm against wiring). */
#define FALLSENA_I2C_SDA_GPIO      22

/* I2C SCL line. Default: GPIO7 (OWNER: confirm against wiring). */
#define FALLSENA_I2C_SCL_GPIO      23

/* I2C bus clock. 400 kHz fast-mode is supported by the MPU6050. */
#define FALLSENA_I2C_FREQ_HZ       400000

/* MPU6050 7-bit I2C address. 0x68 when AD0 is low, 0x69 when AD0 is high. */
#define FALLSENA_MPU6050_ADDR      0x68

/* ------------------------------------------------------------------ */
/* RGB LED - analog 3-pin PWM via LEDC (USED, owner-confirmable)       */
/* ------------------------------------------------------------------ */

/* OWNER: confirm. Common-cathode 3-pin RGB LED, one GPIO per channel.
 * Driven by rgb_led.c via LEDC (one PWM channel per color). Polarity
 * (common-cathode vs -anode) is set in rgb_led.c. */
#define FALLSENA_RGB_LED_R_GPIO    0  /* Red channel   (OWNER: confirm) */
#define FALLSENA_RGB_LED_G_GPIO    20  /* Green channel (OWNER: confirm) */
#define FALLSENA_RGB_LED_B_GPIO    18  /* Blue channel  (OWNER: confirm) */

/* ------------------------------------------------------------------ */
/* Coin vibration motor - GPIO on/off (USED, owner-confirmable)       */
/* ------------------------------------------------------------------ */

/* OWNER: confirm. Driven via transistor/MOSFET, never direct from GPIO.
 * Driven by vibration.c (active-high GPIO output). */
#define FALLSENA_VIBRATION_GPIO    21  /* Vibration motor (OWNER: confirm) */

/* ------------------------------------------------------------------ */
/* Push button - debounced short/long press (USED, owner-confirmable) */
/* ------------------------------------------------------------------ */

/* OWNER: confirm. Active-low to GND, internal pull-up enabled by button.c.
 * NOTE: GPIO9 is an ESP32-C6 strapping pin and must be HIGH at boot; the
 * pull-up keeps it high when the button is released, but holding the button
 * across a reset will force download mode. */
#define FALLSENA_BUTTON_GPIO       1   /* Push button (OWNER: confirm, strapping) */

/* ------------------------------------------------------------------ */
/* Battery voltage sensing (USED, owner-confirmable)                  */
/* ------------------------------------------------------------------ */

/* OWNER: confirm. Must be an ADC1-capable pin (GPIO0..6 on ESP32-C6).
 * Driven by battery.c via the ADC oneshot driver. GPIO3 maps to ADC1
 * channel 3 on the ESP32-C6. The battery+ is read through an on-board
 * resistor voltage divider; set BATTERY_DIVIDER_RATIO in battery.c to
 * match the actual resistors. */
#define FALLSENA_BATTERY_ADC_GPIO  2   /* Battery divider tap (OWNER: confirm) */
