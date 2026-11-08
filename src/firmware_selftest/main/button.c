/*
 * button.c - Push-button driver implementation (debounce + short/long press).
 *
 * See button.h for the public interface and API rationale. Active-low input
 * with internal pull-up; a small polled state machine handles debouncing and
 * distinguishes short vs long presses.
 *
 * STRAPPING-PIN NOTE: on the ESP32-C6, GPIO9 is a strapping pin (it selects
 * the boot mode and must be HIGH at reset to boot from flash). The internal
 * pull-up here keeps it high at boot, so an idle (released) button is safe.
 * However, holding the button DOWN across a reset pulls GPIO9 low and forces
 * download/boot mode -- flag this to the hardware owner.
 */

#include "button.h"
#include "pins.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_timer.h"

/* Debounce window: input must be stable this long before a change is accepted. */
#define BUTTON_DEBOUNCE_MS    25

static const char *TAG = "button";

/* Logical pressed state after debouncing (true == currently held down). */
static bool s_pressed;
/* Raw level last seen, and when it was first seen (for debounce timing). */
static int s_last_raw;
static int64_t s_last_change_us;
/* Timestamp when the (debounced) press began. */
static int64_t s_press_start_us;
/* True once a LONG event has been emitted for the current hold. */
static bool s_long_fired;

/* Read the debounced, active-low button as a boolean "pressed". */
static int button_raw_pressed(void)
{
    /* Active-low: 0 == pressed. */
    return gpio_get_level(FALLSENA_BUTTON_GPIO) == 0;
}

esp_err_t button_init(void)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << FALLSENA_BUTTON_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,    /* idle high, active-low */
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio_config failed: %s", esp_err_to_name(err));
        return err;
    }

    s_pressed        = false;
    s_long_fired     = false;
    s_last_raw       = button_raw_pressed();
    s_last_change_us = esp_timer_get_time();
    s_press_start_us = 0;

    ESP_LOGI(TAG, "button ready (GPIO%d, active-low, pull-up, long>=%dms)",
             FALLSENA_BUTTON_GPIO, BUTTON_LONG_PRESS_MS);
    return ESP_OK;
}

button_event_t button_get_event(void)
{
    const int64_t now_us = esp_timer_get_time();
    const int raw = button_raw_pressed();

    /* Track raw transitions for debounce timing. */
    if (raw != s_last_raw) {
        s_last_raw = raw;
        s_last_change_us = now_us;
    }

    const bool stable =
        (now_us - s_last_change_us) >= (BUTTON_DEBOUNCE_MS * 1000);

    button_event_t event = BUTTON_EVENT_NONE;

    if (stable && (bool)raw != s_pressed) {
        /* Debounced edge. */
        s_pressed = raw;
        if (s_pressed) {
            /* Press begins. */
            s_press_start_us = now_us;
            s_long_fired = false;
        } else {
            /* Release: emit SHORT only if LONG was not already reported. */
            if (!s_long_fired) {
                event = BUTTON_EVENT_SHORT;
            }
        }
    }

    /* While held, fire LONG once the threshold is crossed. */
    if (s_pressed && !s_long_fired &&
        (now_us - s_press_start_us) >= ((int64_t)BUTTON_LONG_PRESS_MS * 1000)) {
        s_long_fired = true;
        event = BUTTON_EVENT_LONG;
    }

    return event;
}
