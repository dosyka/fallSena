/*
 * vibration.c - Coin vibration motor driver implementation.
 *
 * See vibration.h for the public interface. Simple GPIO on/off control.
 */

#include "vibration.h"
#include "pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "vibration";

esp_err_t vibration_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << FALLSENA_VIBRATION_GPIO),
        .mode         = GPIO_MODE_OUTPUT,
        .pull_up_en   = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Start with the motor off. */
    err = gpio_set_level(FALLSENA_VIBRATION_GPIO, 0);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "vibration motor ready (GPIO%d, active-high)",
             FALLSENA_VIBRATION_GPIO);
    return ESP_OK;
}

esp_err_t vibration_on(void)
{
    return gpio_set_level(FALLSENA_VIBRATION_GPIO, 1);
}

esp_err_t vibration_off(void)
{
    return gpio_set_level(FALLSENA_VIBRATION_GPIO, 0);
}

esp_err_t vibration_pulse(uint32_t ms)
{
    esp_err_t err = vibration_on();
    if (err != ESP_OK) {
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(ms));   /* NOTE: blocks the calling task */
    return vibration_off();
}
