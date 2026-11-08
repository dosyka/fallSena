/*
 * rgb_led.c - Analog (PWM) RGB LED driver implementation.
 *
 * See rgb_led.h for the public interface. Uses the ESP-IDF LEDC peripheral
 * (driver/ledc.h): one shared timer plus three channels (one per color).
 */

#include "rgb_led.h"
#include "pins.h"

#include "driver/ledc.h"
#include "esp_log.h"

/*
 * Wiring polarity. Default common-cathode: a higher duty drives more current
 * and therefore more brightness. For a common-anode LED the channel GPIO sinks
 * current, so the duty must be inverted (255 -> off, 0 -> full bright). The
 * physical wiring is NOT confirmed yet -- flip this single line if needed.
 */
#define RGB_LED_COMMON_ANODE  0

/* LEDC speed mode, timer, and resolution shared by all three channels. */
#define RGB_LED_SPEED_MODE    LEDC_LOW_SPEED_MODE
#define RGB_LED_TIMER         LEDC_TIMER_0
#define RGB_LED_DUTY_RES      LEDC_TIMER_8_BIT   /* 0..255 duty, matches input */
#define RGB_LED_FREQ_HZ       5000               /* flicker-free for the eye   */
#define RGB_LED_DUTY_MAX      255                /* (1 << 8) - 1               */

/* One LEDC channel per color. */
#define RGB_LED_CH_R          LEDC_CHANNEL_0
#define RGB_LED_CH_G          LEDC_CHANNEL_1
#define RGB_LED_CH_B          LEDC_CHANNEL_2

static const char *TAG = "rgb_led";

/* Map an 8-bit color component to a polarity-corrected LEDC duty value. */
static inline uint32_t rgb_led_duty(uint8_t value)
{
#if RGB_LED_COMMON_ANODE
    return (uint32_t)(RGB_LED_DUTY_MAX - value);
#else
    return (uint32_t)value;
#endif
}

/* Configure a single LEDC channel bound to the shared timer. */
static esp_err_t rgb_led_config_channel(ledc_channel_t channel, int gpio)
{
    const ledc_channel_config_t ch_cfg = {
        .speed_mode = RGB_LED_SPEED_MODE,
        .channel    = channel,
        .timer_sel  = RGB_LED_TIMER,
        .intr_type  = LEDC_INTR_DISABLE,
        .gpio_num   = gpio,
        /* Start dark regardless of polarity. */
        .duty       = rgb_led_duty(0),
        .hpoint     = 0,
    };
    return ledc_channel_config(&ch_cfg);
}

esp_err_t rgb_led_init(void)
{
    esp_err_t err;

    const ledc_timer_config_t timer_cfg = {
        .speed_mode      = RGB_LED_SPEED_MODE,
        .timer_num       = RGB_LED_TIMER,
        .duty_resolution = RGB_LED_DUTY_RES,
        .freq_hz         = RGB_LED_FREQ_HZ,
        .clk_cfg         = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&timer_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = rgb_led_config_channel(RGB_LED_CH_R, FALLSENA_RGB_LED_R_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "R channel config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = rgb_led_config_channel(RGB_LED_CH_G, FALLSENA_RGB_LED_G_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "G channel config failed: %s", esp_err_to_name(err));
        return err;
    }
    err = rgb_led_config_channel(RGB_LED_CH_B, FALLSENA_RGB_LED_B_GPIO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "B channel config failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "RGB LED ready (R=%d G=%d B=%d, %s, %d Hz)",
             FALLSENA_RGB_LED_R_GPIO, FALLSENA_RGB_LED_G_GPIO,
             FALLSENA_RGB_LED_B_GPIO,
             RGB_LED_COMMON_ANODE ? "common-anode" : "common-cathode",
             RGB_LED_FREQ_HZ);

    return rgb_led_off();
}

/* Apply one channel's duty and latch it. */
static esp_err_t rgb_led_apply(ledc_channel_t channel, uint8_t value)
{
    esp_err_t err = ledc_set_duty(RGB_LED_SPEED_MODE, channel,
                                  rgb_led_duty(value));
    if (err != ESP_OK) {
        return err;
    }
    return ledc_update_duty(RGB_LED_SPEED_MODE, channel);
}

esp_err_t rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    esp_err_t err = rgb_led_apply(RGB_LED_CH_R, r);
    if (err != ESP_OK) {
        return err;
    }
    err = rgb_led_apply(RGB_LED_CH_G, g);
    if (err != ESP_OK) {
        return err;
    }
    return rgb_led_apply(RGB_LED_CH_B, b);
}

esp_err_t rgb_led_off(void)
{
    return rgb_led_set(0, 0, 0);
}

esp_err_t rgb_led_set_color(rgb_led_color_t color)
{
    switch (color) {
    case RGB_LED_OFF:     return rgb_led_set(0,   0,   0);
    case RGB_LED_RED:     return rgb_led_set(255, 0,   0);
    case RGB_LED_GREEN:   return rgb_led_set(0,   255, 0);
    case RGB_LED_BLUE:    return rgb_led_set(0,   0,   255);
    case RGB_LED_YELLOW:  return rgb_led_set(255, 255, 0);
    case RGB_LED_CYAN:    return rgb_led_set(0,   255, 255);
    case RGB_LED_MAGENTA: return rgb_led_set(255, 0,   255);
    case RGB_LED_WHITE:   return rgb_led_set(255, 255, 255);
    default:              return ESP_ERR_INVALID_ARG;
    }
}
