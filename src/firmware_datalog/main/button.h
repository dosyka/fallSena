/*
 * button.h - Push-button driver (debounce + short/long press) for FallSena.
 *
 * The button is wired active-low to GND with the internal pull-up enabled, so
 * the GPIO reads high when released and low when pressed.
 *
 * API style: POLLING. Call button_get_event() periodically (e.g. once per
 * iteration of the main 50 Hz loop). It runs a small debounced state machine
 * internally and returns an event when a completed press is detected:
 *   - BUTTON_EVENT_SHORT  press shorter than the long-press threshold
 *   - BUTTON_EVENT_LONG   press held >= the long-press threshold
 *   - BUTTON_EVENT_NONE   nothing completed since the last call
 *
 * A LONG event fires as soon as the hold threshold is crossed (while still
 * held); the subsequent release does NOT also produce a SHORT event.
 *
 * The polling API was chosen over an ISR/callback so the IMU loop stays simple
 * and timing stays deterministic with no extra synchronization.
 */

#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Press held this long (ms) or more counts as a LONG press. */
#define BUTTON_LONG_PRESS_MS  800

typedef enum {
    BUTTON_EVENT_NONE = 0,
    BUTTON_EVENT_SHORT,
    BUTTON_EVENT_LONG,
} button_event_t;

/*
 * Configure the button GPIO as an input with the internal pull-up enabled.
 * Returns ESP_OK on success.
 */
esp_err_t button_init(void);

/*
 * Poll the button and advance the debounced state machine. Returns the event
 * detected since the previous call (usually BUTTON_EVENT_NONE).
 */
button_event_t button_get_event(void);

#ifdef __cplusplus
}
#endif
