/*
 * mpu6050.c - MPU6050 6-axis IMU driver implementation.
 *
 * See mpu6050.h for the public interface. Uses the ESP-IDF new I2C master
 * driver (driver/i2c_master.h).
 */

#include "mpu6050.h"
#include "pins.h"

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* MPU6050 register map (subset used here). */
#define MPU6050_REG_PWR_MGMT_1   0x6B  /* power management / clock / SLEEP  */
#define MPU6050_REG_GYRO_CONFIG  0x1B  /* gyro full-scale range select      */
#define MPU6050_REG_ACCEL_CONFIG 0x1C  /* accel full-scale range select     */
#define MPU6050_REG_WHO_AM_I     0x75  /* device identity, reads 0x68       */
#define MPU6050_REG_ACCEL_XOUT_H 0x3B  /* first data register of the burst  */
#define MPU6050_WHO_AM_I_EXPECT  0x68

/* Full-scale range configuration values. */
#define MPU6050_ACCEL_FS_16G     0x18  /* AFS_SEL=3 -> +/-16 g     */
#define MPU6050_GYRO_FS_1000DPS  0x10  /* FS_SEL=2  -> +/-1000 dps */

/* PWR_MGMT_1 bit fields. */
#define MPU6050_PWR1_SLEEP_BIT   (1 << 6)

/*
 * Sensitivity scale factors for the configured full-scale ranges.
 *   Accel +/-16 g     -> 2048 LSB/g
 *   Gyro  +/-1000 dps -> 32.8 LSB/(deg/s)
 */
#define MPU6050_ACCEL_LSB_PER_G   2048.0f
#define MPU6050_GYRO_LSB_PER_DPS  32.8f

#define MPU6050_I2C_TIMEOUT_MS    1000

static const char *TAG = "mpu6050";

static i2c_master_bus_handle_t s_bus_handle;
static i2c_master_dev_handle_t s_dev_handle;

/* Write a single byte to a device register. */
static esp_err_t mpu6050_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = { reg, val };
    return i2c_master_transmit(s_dev_handle, buf, sizeof(buf),
                               MPU6050_I2C_TIMEOUT_MS);
}

/* Read `len` bytes starting at `reg` (register pointer write + read). */
static esp_err_t mpu6050_read_regs(uint8_t reg, uint8_t *data, size_t len)
{
    return i2c_master_transmit_receive(s_dev_handle, &reg, 1, data, len,
                                       MPU6050_I2C_TIMEOUT_MS);
}

esp_err_t mpu6050_init(void)
{
    esp_err_t err;

    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = FALLSENA_I2C_PORT_NUM,
        .sda_io_num = FALLSENA_I2C_SDA_GPIO,
        .scl_io_num = FALLSENA_I2C_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };

    err = i2c_new_master_bus(&bus_cfg, &s_bus_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = FALLSENA_MPU6050_ADDR,
        .scl_speed_hz = FALLSENA_I2C_FREQ_HZ,
    };

    err = i2c_master_bus_add_device(s_bus_handle, &dev_cfg, &s_dev_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_master_bus_add_device failed: %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Wake the device: clear the SLEEP bit (chip boots into sleep). */
    err = mpu6050_write_reg(MPU6050_REG_PWR_MGMT_1, 0x00);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to write PWR_MGMT_1 (wake): %s",
                 esp_err_to_name(err));
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(100)); /* allow the sensor to stabilize */

    /* Configure accelerometer full-scale range to +/-16 g. */
    err = mpu6050_write_reg(MPU6050_REG_ACCEL_CONFIG, MPU6050_ACCEL_FS_16G);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to write ACCEL_CONFIG (+/-16g): %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Configure gyroscope full-scale range to +/-1000 dps. */
    err = mpu6050_write_reg(MPU6050_REG_GYRO_CONFIG, MPU6050_GYRO_FS_1000DPS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to write GYRO_CONFIG (+/-1000dps): %s",
                 esp_err_to_name(err));
        return err;
    }

    /* Verify identity. */
    uint8_t who = 0;
    err = mpu6050_read_regs(MPU6050_REG_WHO_AM_I, &who, 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to read WHO_AM_I: %s", esp_err_to_name(err));
        return err;
    }
    if (who != MPU6050_WHO_AM_I_EXPECT) {
        ESP_LOGE(TAG,
                 "WHO_AM_I mismatch: got 0x%02X, expected 0x%02X "
                 "(check wiring / address)",
                 who, MPU6050_WHO_AM_I_EXPECT);
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG,
             "MPU6050 ready (WHO_AM_I=0x%02X, accel +/-16g, gyro +/-1000dps)",
             who);
    return ESP_OK;
}

esp_err_t mpu6050_read(mpu6050_raw_data_t *raw, mpu6050_scaled_data_t *scaled)
{
    /*
     * Burst-read 14 bytes from ACCEL_XOUT_H: accel X/Y/Z, temp, gyro X/Y/Z.
     * We skip the 2 temperature bytes (indices 6,7).
     */
    uint8_t buf[14];
    esp_err_t err = mpu6050_read_regs(MPU6050_REG_ACCEL_XOUT_H, buf,
                                      sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    const int16_t ax = (int16_t)((buf[0]  << 8) | buf[1]);
    const int16_t ay = (int16_t)((buf[2]  << 8) | buf[3]);
    const int16_t az = (int16_t)((buf[4]  << 8) | buf[5]);
    /* buf[6], buf[7] = temperature (unused) */
    const int16_t gx = (int16_t)((buf[8]  << 8) | buf[9]);
    const int16_t gy = (int16_t)((buf[10] << 8) | buf[11]);
    const int16_t gz = (int16_t)((buf[12] << 8) | buf[13]);

    if (raw) {
        raw->accel_x = ax;
        raw->accel_y = ay;
        raw->accel_z = az;
        raw->gyro_x  = gx;
        raw->gyro_y  = gy;
        raw->gyro_z  = gz;
    }

    if (scaled) {
        scaled->accel_x = ax / MPU6050_ACCEL_LSB_PER_G;
        scaled->accel_y = ay / MPU6050_ACCEL_LSB_PER_G;
        scaled->accel_z = az / MPU6050_ACCEL_LSB_PER_G;
        scaled->gyro_x  = gx / MPU6050_GYRO_LSB_PER_DPS;
        scaled->gyro_y  = gy / MPU6050_GYRO_LSB_PER_DPS;
        scaled->gyro_z  = gz / MPU6050_GYRO_LSB_PER_DPS;
    }

    return ESP_OK;
}
