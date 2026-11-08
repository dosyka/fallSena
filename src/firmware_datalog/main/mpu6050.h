/*
 * mpu6050.h - Minimal MPU6050 (6-axis IMU) driver for the FallSena firmware.
 *
 * Uses the ESP-IDF "new" I2C master driver (driver/i2c_master.h), supported
 * in ESP-IDF v5.2+ / v6.x. The legacy driver/i2c.h API is intentionally NOT
 * used.
 *
 * DATA-LOGGING VARIANT configuration after mpu6050_init():
 *   - Device awake (SLEEP bit cleared in PWR_MGMT_1).
 *   - Accelerometer full-scale range: +/-8 g       (AFS_SEL=2, ACCEL_CONFIG 0x1C).
 *   - Gyroscope full-scale range:     +/-1000 deg/s (FS_SEL=2,  GYRO_CONFIG  0x1B).
 *
 * The wider ranges (vs the +/-2 g / +/-250 dps used by the main firmware) keep
 * hard impacts from clipping while recording labeled fall data for tuning.
 */

#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Raw 16-bit two's-complement readings straight from the sensor registers. */
typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} mpu6050_raw_data_t;

/* Scaled readings: acceleration in g, angular rate in deg/s. */
typedef struct {
    float accel_x;  /* g      */
    float accel_y;  /* g      */
    float accel_z;  /* g      */
    float gyro_x;   /* deg/s  */
    float gyro_y;   /* deg/s  */
    float gyro_z;   /* deg/s  */
} mpu6050_scaled_data_t;

/*
 * Initialize the I2C master bus, attach the MPU6050, wake it, and verify
 * WHO_AM_I (reg 0x75 == 0x68).
 *
 * Returns:
 *   ESP_OK              on success.
 *   ESP_ERR_NOT_FOUND   if WHO_AM_I does not match (wrong/absent device).
 *   other esp_err_t     on I2C / bus-setup failure.
 */
esp_err_t mpu6050_init(void);

/*
 * Read all 6 axes (accel X/Y/Z + gyro X/Y/Z) in a single burst from the data
 * registers starting at 0x3B.
 *
 * Either output pointer may be NULL if that representation is not needed, but
 * at least one should be non-NULL to be useful.
 */
esp_err_t mpu6050_read(mpu6050_raw_data_t *raw, mpu6050_scaled_data_t *scaled);

#ifdef __cplusplus
}
#endif
