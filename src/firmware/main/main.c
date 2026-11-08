/*
 * FallSena fall-detection wearable firmware.
 *
 * Phase 1 raw-data logging: initializes the MPU6050 IMU over I2C and streams
 * raw accel + gyro readings over serial at a fixed sample rate.
 *
 * Also runs a minimal user-peripheral demo (RGB LED, vibration motor, button)
 * polled inside the same 50 Hz loop so the IMU stream is not disrupted:
 *   - SHORT button press: short vibration pulse + flash the LED green.
 *   - LONG  button press: set the LED blue.
 *
 * Battery voltage / state-of-charge is read via the ADC and logged at a LOW
 * rate (a few seconds) so it does not spam the 50 Hz IMU stream.
 *
 * Phase 2 fall detection (#FW3): each 50 Hz sample's scaled accel + gyro is fed
 * into the fall_detector state machine. On a NEW suspected-fall edge the loop
 * logs a prominent "FALL SUSPECTED" line.
 *
 * Phase 2 user-cancel countdown (#FW4): a suspected-fall edge no longer jumps
 * straight to a confirmed alert. Instead the loop runs an explicit alert
 * lifecycle (NORMAL -> COUNTDOWN -> CONFIRMED) entirely from the 50 Hz loop,
 * tracked in iteration counts like the rest of the code:
 *   - COUNTDOWN: a cancel window (FALL_CANCEL_WINDOW_MS) during which the user
 *     gets a distinct alert (blinking red LED + a long, insistent "about to
 *     alert" vibration buzz: a non-blocking on/off cycle with the motor fully
 *     ON, iteration-counted from the 50 Hz loop). A SHORT button press
 *     cancels: the detector is reset, the buzz is stopped, a brief
 *     green-LED + vibration acknowledgement plays, and we return to NORMAL.
 *   - CONFIRMED: reached if the window expires with no cancel. Logs a
 *     prominent confirmed line, latches a solid red LED, and drives the
 *     vibration motor as an escalating alarm: a short pulse pattern
 *     (FALL_CONFIRMED_VIBE_PULSES on/off pulses) followed by the motor
 *     staying solid ON for the rest of the alarm. The pulse pattern is
 *     iteration-counted from the 50 Hz loop (no blocking pulses), like the
 *     COUNTDOWN cues. This is where the BLE fall-event notify (#FW5) hooks in.
 *     A LONG button press clears the alert (detector reset, LED off, motor
 *     off, NORMAL).
 * While in COUNTDOWN/CONFIRMED the button belongs to the alert flow, so the
 * NORMAL-phase demo button behavior is suppressed.
 *
 * Phase 3 BLE (#FW5): a NimBLE GATT server (ble_service.c) advertises as
 * "FallSena" and lets the caretaker app connect. On a CONFIRMED fall it pushes
 * a notification on the Fall-Event characteristic; the low-rate battery block
 * also feeds the standard Battery Service + a custom device-status
 * characteristic. BLE is best-effort: init failure logs a warning and the fall
 * detector (core function) still runs. Notify calls are non-blocking and safe
 * whether or not a client is connected.
 *
 * Low-battery indicator (#FW-BATTLOW): the low-rate battery block also tracks a
 * `battery_low` flag with hysteresis (enter at <= BATTERY_LOW_ENTER_PCT, exit at
 * >= BATTERY_LOW_EXIT_PCT) plus a small debounce (BATTERY_LOW_DEBOUNCE_READS
 * consecutive qualifying reads, ~5 s apart) so a single noisy sample can't
 * toggle it. While low, the loop plays a low-duty periodic YELLOW blink (a short
 * BATTERY_LOW_BLINK_ON_MS pulse once every BATTERY_LOW_BLINK_PERIOD_MS),
 * iteration-counted like the FALL_* timers and fully non-blocking. The blink is
 * strictly subordinate to the LED: it drives the LED ONLY while
 * alert_phase == ALERT_NORMAL and no green demo flash is active; an active fall
 * alert (COUNTDOWN/CONFIRMED) always owns the LED. The LED is returned to off
 * after each blink and when `battery_low` clears. This is the INDICATOR only.
 *
 * Over-discharge protective cutoff (#FW-BATTPROT): SEPARATE from and deeper than
 * the #FW-BATTLOW indicator. The low-rate battery block also tracks a critical
 * streak on the TRUE terminal voltage from battery_read_mv() (not the rough
 * %-curve): once mv <= BATTERY_CRITICAL_MV for BATTERY_CRITICAL_DEBOUNCE_READS
 * consecutive reads (~15 s sustained, so a transient sag under the alarm motor
 * can't trip it) AND the device is idle (alert_phase == ALERT_NORMAL), the loop
 * runs a protective shutdown: log loudly, quiesce the motor + LED, arm the push
 * button as an ESP32-C6 LP/RTC-GPIO deep-sleep wake source, and enter deep sleep
 * to stop draining the cell. An active fall alert (COUNTDOWN/CONFIRMED) always
 * outranks battery protection, so the cutoff never fires mid-alarm. A boot-time
 * re-check does the same before starting the alarm system on a dying cell, so
 * after charging the user presses the button to wake and, if still critical, the
 * device just re-sleeps. A failed battery read never triggers a shutdown.
 *
 * Out of scope for this build (later tasks): cloud/FCM push.
 */

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_sleep.h"

#include "pins.h"

#include "mpu6050.h"
#include "rgb_led.h"
#include "vibration.h"
#include "button.h"
#include "battery.h"
#include "fall_detector.h"
#include "ble_service.h"

#define FALLSENA_FW_VERSION  "0.5.0"

/* IMU streaming sample rate. */
#define FALLSENA_SAMPLE_RATE_HZ  50
#define FALLSENA_SAMPLE_PERIOD_MS (1000 / FALLSENA_SAMPLE_RATE_HZ)

/* Demo: vibration pulse length on a short press (ms, briefly blocks the loop). */
#define FALLSENA_DEMO_PULSE_MS   150
/* Demo: how many 50 Hz iterations to keep the green flash lit (~300 ms). */
#define FALLSENA_DEMO_FLASH_ITERS 15

/* Battery log cadence: log mV + % once every N loop iterations. At 50 Hz,
 * 250 iterations == every 5 s (NOT at the 50 Hz IMU rate). */
#define FALLSENA_BATTERY_LOG_ITERS 250

/*
 * #FW-BATTLOW low-battery indicator (indicator ONLY; no deep-sleep/cutoff).
 * `battery_low` is updated in the low-rate battery block with hysteresis so it
 * does not flicker at the boundary: enter low at <= ENTER, exit low at >= EXIT.
 * A small debounce (BATTERY_LOW_DEBOUNCE_READS consecutive qualifying reads,
 * ~5 s apart at the battery cadence above) stops one noisy sample toggling it.
 */
#define BATTERY_LOW_ENTER_PCT       20
#define BATTERY_LOW_EXIT_PCT        25
#define BATTERY_LOW_DEBOUNCE_READS  2

/*
 * #FW-BATTPROT protective over-discharge cutoff (SEPARATE from and DEEPER than
 * the #FW-BATTLOW indicator above; leaves that indicator untouched). This uses
 * the TRUE battery-terminal voltage in mV from battery_read_mv() -- NOT the
 * rough %-curve -- so the threshold is a real voltage floor. 3.3 V is a
 * conservative safe floor for a 3.7 V LiPo. A 3-read debounce at the ~5 s
 * battery cadence (~15 s sustained) stops a transient sag under load -- e.g. the
 * vibration motor firing during a CONFIRMED alarm -- from tripping the cutoff.
 */
#define BATTERY_CRITICAL_MV             3300
#define BATTERY_CRITICAL_DEBOUNCE_READS 3
/* Fallback only: if the button deep-sleep wake source cannot be armed, wake on a
 * timer every this many minutes to re-evaluate the cell instead. */
#define BATTERY_CRITICAL_TIMER_WAKE_MIN 5

/*
 * Low-battery LED cue: a short, low-duty YELLOW blink so it is noticeable but
 * not annoying and cheap on power (a ~120 ms pulse once every ~3 s). Expressed
 * in ms and converted to 50 Hz loop iterations, matching the FALL_* timers.
 */
#define BATTERY_LOW_BLINK_PERIOD_MS 3000
#define BATTERY_LOW_BLINK_ON_MS     120
#define BATTERY_LOW_BLINK_ON_ITERS  (BATTERY_LOW_BLINK_ON_MS / FALLSENA_SAMPLE_PERIOD_MS)
#define BATTERY_LOW_BLINK_OFF_ITERS \
    ((BATTERY_LOW_BLINK_PERIOD_MS - BATTERY_LOW_BLINK_ON_MS) / FALLSENA_SAMPLE_PERIOD_MS)

/*
 * #FW4 user-cancel countdown timing. Everything is expressed in ms and then
 * converted to 50 Hz loop iterations, matching the iteration-counted style
 * used elsewhere in this loop.
 */
/* Cancel window: time the user has to cancel a suspected fall before it is
 * treated as CONFIRMED. */
#define FALL_CANCEL_WINDOW_MS       10000
#define FALL_CANCEL_WINDOW_ITERS    (FALL_CANCEL_WINDOW_MS / FALLSENA_SAMPLE_PERIOD_MS)

/* COUNTDOWN LED blink: toggle the red LED every this many ms (200 ms -> ~2.5
 * Hz blink), so the alert is visually distinct from a solid-red state. */
#define FALL_COUNTDOWN_BLINK_MS     200
#define FALL_COUNTDOWN_BLINK_ITERS  (FALL_COUNTDOWN_BLINK_MS / FALLSENA_SAMPLE_PERIOD_MS)

/* COUNTDOWN vibration cue: the grace-period "about to alert" buzz. A LONG,
 * insistent on/off cycle (motor fully ON, since it is on/off only) so the
 * wearer clearly feels that a caretaker notification is imminent. Driven
 * NON-BLOCKING from the 50 Hz loop (iteration-counted, like the CONFIRMED
 * escalation) so it never stalls IMU sampling or the cancel-button check.
 * Tunable: bump ON up / OFF down to make it even more insistent. */
#define FALL_COUNTDOWN_VIBE_ON_MS   500
#define FALL_COUNTDOWN_VIBE_ON_ITERS \
    (FALL_COUNTDOWN_VIBE_ON_MS / FALLSENA_SAMPLE_PERIOD_MS)
#define FALL_COUNTDOWN_VIBE_OFF_MS  500
#define FALL_COUNTDOWN_VIBE_OFF_ITERS \
    (FALL_COUNTDOWN_VIBE_OFF_MS / FALLSENA_SAMPLE_PERIOD_MS)

/*
 * CONFIRMED vibration escalation. On entering CONFIRMED the motor plays a
 * short pulse pattern (FALL_CONFIRMED_VIBE_PULSES distinct on/off pulses) and
 * then latches solid ON for the rest of the alarm. Driven non-blocking from
 * the 50 Hz loop (iteration-counted, like the COUNTDOWN cues) so it never
 * stalls the IMU cadence.
 */
#define FALL_CONFIRMED_VIBE_PULSES   3
#define FALL_CONFIRMED_VIBE_ON_MS    200
#define FALL_CONFIRMED_VIBE_ON_ITERS \
    (FALL_CONFIRMED_VIBE_ON_MS / FALLSENA_SAMPLE_PERIOD_MS)
#define FALL_CONFIRMED_VIBE_OFF_MS   200
#define FALL_CONFIRMED_VIBE_OFF_ITERS \
    (FALL_CONFIRMED_VIBE_OFF_MS / FALLSENA_SAMPLE_PERIOD_MS)

/*
 * #FW5-fix CONFIRMED alert auto-clear. A CONFIRMED alert that is never manually
 * LONG-press cleared would latch the red LED, the alarm motor, and (critically)
 * the BLE Fall-Event flag at 1 forever -- and the app only re-arms once it sees
 * the flag go back to 0. So after this timeout with no manual clear, the loop
 * runs the same clear sequence as the LONG-press path (which now also resets the
 * BLE flag). This is the alarm / fall-flag lifetime and is tunable; expressed in
 * ms and converted to 50 Hz loop iterations like the FALL_* timers above.
 */
#define FALL_CONFIRMED_AUTOCLEAR_MS  30000
#define FALL_CONFIRMED_AUTOCLEAR_ITERS \
    (FALL_CONFIRMED_AUTOCLEAR_MS / FALLSENA_SAMPLE_PERIOD_MS)

/*
 * Alert lifecycle phase, driven from the main loop. NORMAL keeps the demo
 * button behavior; COUNTDOWN and CONFIRMED hand the button to the alert flow.
 */
typedef enum {
    ALERT_NORMAL = 0,   /* no active alert; demo button behavior applies      */
    ALERT_COUNTDOWN,    /* fall suspected; user can still cancel              */
    ALERT_CONFIRMED,    /* cancel window expired; fall treated as confirmed  */
} alert_phase_t;

static const char *TAG = "fallsena";

/*
 * #FW-BATTPROT protective shutdown. Called only when the cell is confirmed
 * critically low (battery_read_mv() <= BATTERY_CRITICAL_MV) and the device is
 * idle (ALERT_NORMAL). Logs loudly, quiesces the outputs so nothing keeps
 * draining the dying cell, arms a deep-sleep wake source, and enters deep sleep.
 * Does NOT return.
 *
 * Wake source: ESP32-C6 LP/RTC-GPIO deep-sleep wake on the push button
 * (FALLSENA_BUTTON_GPIO = GPIO1, within the C6 RTC-capable wake range 0..7). The
 * button is active-low (idle high via pull-up), so we wake on a LOW level. The
 * ESP_SLEEP_GPIO_ENABLE_INTERNAL_RESISTORS sdkconfig option (default on) applies
 * the internal pull-up for the wake pin during deep sleep. If arming the button
 * wake fails, we fall back to a periodic timer wake that re-evaluates the cell.
 * After charging, a button press wakes the device and the boot-time re-check
 * re-sleeps if the cell is still critical.
 */
static void battery_enter_protective_sleep(int mv)
{
    ESP_LOGE(TAG, "battery CRITICAL (%d mV <= %d mV): entering protective deep "
                  "sleep to prevent over-discharge; press the button to wake "
                  "after charging", mv, BATTERY_CRITICAL_MV);

    /* Quiesce outputs so nothing keeps draining the dying cell. */
    vibration_off();
    rgb_led_off();

    /* Arm the button as the deep-sleep wake source (wake on LOW == pressed). */
    esp_err_t werr = esp_sleep_enable_gpio_wakeup_on_hp_periph_powerdown(
            1ULL << FALLSENA_BUTTON_GPIO, ESP_GPIO_WAKEUP_GPIO_LOW);
    if (werr != ESP_OK) {
        ESP_LOGE(TAG, "button deep-sleep wake arm failed (%s); falling back to "
                      "a %d-min timer wake", esp_err_to_name(werr),
                 BATTERY_CRITICAL_TIMER_WAKE_MIN);
        esp_sleep_enable_timer_wakeup(
                (uint64_t)BATTERY_CRITICAL_TIMER_WAKE_MIN * 60ULL * 1000000ULL);
    }

    esp_deep_sleep_start(); /* does not return */
}

/*
 * #FW-BATTPROT boot-time re-check. Runs early in app_main (after battery_init(),
 * before the main loop, while no alert can be active) so we never start the
 * alarm system on a dying cell. To avoid a false shutdown, this only sleeps on a
 * CONFIRMED-critical reading: it requires TWO good reads that are BOTH critical.
 * A battery read error, or any non-critical read, aborts the check and lets the
 * device boot normally.
 */
static void battery_boot_critical_check(bool battery_ok)
{
    if (!battery_ok) {
        return; /* battery subsystem down -> can't judge; never false-trip. */
    }

    /* Let the ADC / divider settle briefly before sampling. */
    vTaskDelay(pdMS_TO_TICKS(100));

    int mv1, mv2;
    if (battery_read_mv(&mv1) != ESP_OK || mv1 > BATTERY_CRITICAL_MV) {
        return; /* read failed or not critical -> boot normally. */
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    if (battery_read_mv(&mv2) != ESP_OK || mv2 > BATTERY_CRITICAL_MV) {
        return; /* second read failed or not critical -> boot normally. */
    }

    ESP_LOGE(TAG, "boot battery re-check CONFIRMED critical (%d/%d mV)",
             mv1, mv2);
    battery_enter_protective_sleep(mv1 < mv2 ? mv1 : mv2);
}

/*
 * #FW5-fix shared CONFIRMED-alert clear sequence, used by BOTH the manual
 * LONG-press clear and the new timeout auto-clear so the two paths can never
 * drift. Resets the fall detector, quiesces the LED + alarm motor, tears down
 * the CONFIRMED vibration-escalation state, clears the BLE Fall-Event flag (so
 * the caretaker app re-arms for the next fall), and returns to ALERT_NORMAL.
 * Operates on the loop-local alert state via pointers.
 */
static void alert_clear_confirmed(alert_phase_t *alert_phase,
                                  int *confirmed_pulses_left,
                                  bool *confirmed_vibe_on)
{
    fall_detector_reset();
    rgb_led_off();
    vibration_off(); /* stop the alarm motor */
    *confirmed_pulses_left = 0;
    *confirmed_vibe_on = false;
    ble_service_notify_fall_cleared(); /* reset BLE flag so the app re-arms */
    *alert_phase = ALERT_NORMAL;
}

/*
 * #P3-USERCHECK-FW shared COUNTDOWN-cancel sequence, used by BOTH the on-device
 * SHORT-press cancel and the new app-issued cancel (Command char) so the two
 * paths can never drift. Resets the fall detector, stops the "about to alert"
 * buzz, plays a brief green-LED + vibration acknowledgement (reusing the
 * NORMAL-phase green-flash timeout to clear the LED), clears the BLE Fall-Event
 * flag (BLE sits at 2/suspected during the countdown, so cancel must push it
 * back to 0 for the app to re-arm), and returns to ALERT_NORMAL. Operates on
 * the loop-local alert state via pointers.
 */
static void alert_cancel_countdown(alert_phase_t *alert_phase,
                                   int *green_flash_iters,
                                   bool *countdown_vibe_on)
{
    fall_detector_reset();
    /* Explicitly stop the countdown "about to alert" buzz so it can't leave the
     * motor latched ON before we hand off to the green-ack pulse below. */
    vibration_off();
    *countdown_vibe_on = false;
    /* Brief green + short vibration acknowledgement, reusing the NORMAL-phase
     * green-flash timeout to clear the LED. */
    rgb_led_set_color(RGB_LED_GREEN);
    *green_flash_iters = FALLSENA_DEMO_FLASH_ITERS;
    vibration_pulse(FALLSENA_DEMO_PULSE_MS); /* briefly blocks */
    ble_service_notify_fall_cleared(); /* BLE 2 -> 0 so the app re-arms */
    *alert_phase = ALERT_NORMAL;
}

void app_main(void)
{
    ESP_LOGI(TAG, "FallSena firmware booting (v%s)", FALLSENA_FW_VERSION);

    esp_err_t err = mpu6050_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "MPU6050 init failed (%s); not streaming.",
                 esp_err_to_name(err));
        /* Idle instead of spamming garbage data. */
        while (1) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    /*
     * Initialize the user-facing peripherals for the demo. These are
     * best-effort: if any fails we still stream the IMU (the core function).
     */
    if (rgb_led_init() != ESP_OK) {
        ESP_LOGW(TAG, "RGB LED init failed; continuing without it");
    }
    if (vibration_init() != ESP_OK) {
        ESP_LOGW(TAG, "vibration init failed; continuing without it");
    }
    if (button_init() != ESP_OK) {
        ESP_LOGW(TAG, "button init failed; continuing without it");
    }
    bool battery_ok = (battery_init() == ESP_OK);
    if (!battery_ok) {
        ESP_LOGW(TAG, "battery init failed; continuing without it");
    }

    /* #FW-BATTPROT: before bringing up the alarm system, bail straight back into
     * protective deep sleep if the cell is already critically low. Guarded to
     * only sleep on a CONFIRMED-critical reading (never on a read error). */
    battery_boot_critical_check(battery_ok);

    /* Bring up the BLE GATT server (best-effort like the peripherals above).
     * If it fails we still stream the IMU and run fall detection. */
    if (ble_service_init() != ESP_OK) {
        ESP_LOGW(TAG, "BLE service init failed; continuing without it");
    }

    /* Core fall-detection state machine (consumes each 50 Hz sample). */
    fall_detector_init();

    ESP_LOGI(TAG, "Streaming IMU at %d Hz", FALLSENA_SAMPLE_RATE_HZ);
    /* CSV header for downstream parsers. */
    ESP_LOGI(TAG, "fmt: ax,ay,az,gx,gy,gz (raw int16) | "
                  "ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps (scaled)");

    /* Countdown of loop iterations remaining for the green flash (0 == off). */
    int green_flash_iters = 0;

    /* Down-counter for the low-rate battery log (fires every N iterations). */
    int battery_log_iters = FALLSENA_BATTERY_LOG_ITERS;

    /* #FW-BATTLOW low-battery indicator state (indicator only, no cutoff). */
    bool battery_low = false;      /* hysteretic+debounced low-battery flag    */
    int batt_low_streak = 0;       /* consecutive qualifying reads (debounce)  */
    /* #FW-BATTPROT protective cutoff: consecutive critical (mV) reads. */
    int batt_crit_streak = 0;
    int batt_blink_iters = 0;      /* iterations until next low-batt blink flip */
    bool batt_blink_on = false;    /* current low-batt blink LED state (yellow) */

    /* #FW4 alert lifecycle state (all iteration-counted at 50 Hz). */
    alert_phase_t alert_phase = ALERT_NORMAL;
    int countdown_iters = 0;       /* iterations left in the cancel window     */
    int blink_iters = 0;           /* iterations until next LED blink toggle   */
    bool blink_on = false;         /* current blink LED state (red on/off)     */
    int countdown_vibe_iters = 0;  /* iters until next COUNTDOWN buzz toggle    */
    bool countdown_vibe_on = false;/* current COUNTDOWN buzz motor state        */
    /* CONFIRMED vibration escalation (pulse-pulse-pulse -> solid on). */
    int confirmed_pulses_left = 0; /* ON pulses still to play (0 == solid on)  */
    int confirmed_vibe_iters = 0;  /* iterations until next pulse on/off toggle */
    bool confirmed_vibe_on = false;/* motor state within the current pulse      */
    int confirmed_autoclear_iters = 0; /* iters until CONFIRMED auto-clears (#FW5-fix) */

    TickType_t last_wake = xTaskGetTickCount();
    while (1) {
        mpu6050_raw_data_t raw;
        mpu6050_scaled_data_t scaled;

        /*
         * Poll the button once per iteration (cheap, no extra task). Who owns
         * the event depends on the alert phase:
         *   - NORMAL:    the demo behavior (SHORT = green flash + vibrate,
         *                LONG = blue).
         *   - COUNTDOWN: SHORT = user cancels the suspected fall.
         *   - CONFIRMED: LONG  = clear the alert.
         */
        button_event_t btn = button_get_event();

        switch (alert_phase) {
        case ALERT_NORMAL:
            /* Demo behavior only applies while no alert is active. */
            switch (btn) {
            case BUTTON_EVENT_SHORT:
                ESP_LOGI(TAG, "button: SHORT press");
                rgb_led_set_color(RGB_LED_GREEN);
                green_flash_iters = FALLSENA_DEMO_FLASH_ITERS;
                vibration_pulse(FALLSENA_DEMO_PULSE_MS); /* briefly blocks */
                break;
            case BUTTON_EVENT_LONG:
                ESP_LOGI(TAG, "button: LONG press");
                green_flash_iters = 0;
                rgb_led_set_color(RGB_LED_BLUE);
                break;
            case BUTTON_EVENT_NONE:
            default:
                break;
            }
            break;

        case ALERT_COUNTDOWN:
            if (btn == BUTTON_EVENT_SHORT) {
                /* User cancel (on-device): abort the suspected fall and re-arm. */
                ESP_LOGW(TAG, "*** FALL CANCELLED *** user cancelled during "
                              "countdown; no alert will be raised");
                alert_cancel_countdown(&alert_phase, &green_flash_iters,
                                       &countdown_vibe_on);
                break;
            }
            /* App cancel (#P3-USERCHECK-FW): the phone wrote the cancel command
             * during the "Are you OK?" grace window. Run the IDENTICAL sequence
             * as the on-device SHORT-press cancel above. */
            if (ble_service_poll_app_cancel()) {
                ESP_LOGW(TAG, "*** FALL CANCELLED *** app cancelled during "
                              "countdown; no alert will be raised");
                alert_cancel_countdown(&alert_phase, &green_flash_iters,
                                       &countdown_vibe_on);
                break;
            }

            /* Distinct COUNTDOWN alert: blink the red LED. */
            if (--blink_iters <= 0) {
                blink_iters = FALL_COUNTDOWN_BLINK_ITERS;
                blink_on = !blink_on;
                if (blink_on) {
                    rgb_led_set_color(RGB_LED_RED);
                } else {
                    rgb_led_off();
                }
            }
            /* Long, insistent "about to alert" buzz: a NON-BLOCKING on/off
             * cycle (motor fully ON) so the wearer clearly feels the imminent
             * notification. Iteration-counted like the CONFIRMED escalation so
             * it never stalls the IMU stream or the cancel-button poll. */
            if (--countdown_vibe_iters <= 0) {
                countdown_vibe_on = !countdown_vibe_on;
                if (countdown_vibe_on) {
                    vibration_on();
                    countdown_vibe_iters = FALL_COUNTDOWN_VIBE_ON_ITERS;
                } else {
                    vibration_off();
                    countdown_vibe_iters = FALL_COUNTDOWN_VIBE_OFF_ITERS;
                }
            }
            /* Cancel window countdown. When it expires, confirm the fall. */
            if (--countdown_iters <= 0) {
                ESP_LOGE(TAG, "*** FALL CONFIRMED *** cancel window elapsed");
                rgb_led_set_color(RGB_LED_RED); /* solid red */
                /* #FW5: notify any subscribed caretaker app. Non-blocking and
                 * a no-op if BLE is down or nobody is connected. */
                ble_service_notify_fall_confirmed();
                /* Start the CONFIRMED vibration escalation: begin the first
                 * ON pulse now; the ALERT_CONFIRMED case drives the rest
                 * non-blocking until it latches solid on. */
                confirmed_pulses_left = FALL_CONFIRMED_VIBE_PULSES;
                confirmed_vibe_on = true;
                confirmed_vibe_iters = FALL_CONFIRMED_VIBE_ON_ITERS;
                vibration_on();
                /* #FW5-fix: arm the auto-clear timeout so a CONFIRMED alert
                 * that is never manually cleared eventually re-arms the app. */
                confirmed_autoclear_iters = FALL_CONFIRMED_AUTOCLEAR_ITERS;
                alert_phase = ALERT_CONFIRMED;
            }
            break;

        case ALERT_CONFIRMED:
            if (btn == BUTTON_EVENT_LONG) {
                ESP_LOGW(TAG, "alert cleared");
                alert_clear_confirmed(&alert_phase, &confirmed_pulses_left,
                                      &confirmed_vibe_on);
                break;
            }
            /*
             * Vibration escalation: play FALL_CONFIRMED_VIBE_PULSES distinct
             * on/off pulses, then latch the motor solid ON for the rest of
             * the alarm. Iteration-counted so it never blocks the 50 Hz loop.
             * confirmed_pulses_left == 0 means the escalation is done (solid
             * on); the guard leaves the motor untouched from then on.
             */
            if (confirmed_pulses_left > 0 && --confirmed_vibe_iters <= 0) {
                if (confirmed_vibe_on) {
                    /* End of an ON pulse -> insert the OFF gap. */
                    confirmed_vibe_on = false;
                    vibration_off();
                    confirmed_vibe_iters = FALL_CONFIRMED_VIBE_OFF_ITERS;
                } else {
                    /* End of an OFF gap: one pulse is complete. */
                    confirmed_pulses_left--;
                    if (confirmed_pulses_left > 0) {
                        /* Start the next pulse. */
                        confirmed_vibe_on = true;
                        vibration_on();
                        confirmed_vibe_iters = FALL_CONFIRMED_VIBE_ON_ITERS;
                    } else {
                        /* All pulses done: solid on for the rest of the alarm. */
                        vibration_on();
                    }
                }
            }
            /*
             * #FW5-fix auto-clear: if the CONFIRMED alert is never manually
             * LONG-press cleared, time it out after FALL_CONFIRMED_AUTOCLEAR_MS
             * and run the SAME clear sequence as the LONG-press path (which
             * also resets the BLE Fall-Event flag so the app re-arms).
             */
            if (--confirmed_autoclear_iters <= 0) {
                ESP_LOGW(TAG, "alert auto-cleared after timeout");
                alert_clear_confirmed(&alert_phase, &confirmed_pulses_left,
                                      &confirmed_vibe_on);
            }
            break;

        default:
            break;
        }

        /* Time out the green flash without blocking the IMU stream. Only
         * meaningful in NORMAL (the alert phases drive the LED directly and
         * leave green_flash_iters at 0). */
        if (green_flash_iters > 0 && --green_flash_iters == 0) {
            rgb_led_off();
        }

        /*
         * #FW-BATTLOW low-battery LED cue: a short, low-duty YELLOW blink while
         * `battery_low`. It may ONLY drive the LED in idle NORMAL with no green
         * demo flash active; an active fall alert (COUNTDOWN/CONFIRMED) always
         * owns the LED, so the indicator never clobbers it. Iteration-counted
         * and non-blocking, like the FALL_* timers.
         */
        if (battery_low && alert_phase == ALERT_NORMAL && green_flash_iters == 0) {
            if (--batt_blink_iters <= 0) {
                batt_blink_on = !batt_blink_on;
                if (batt_blink_on) {
                    rgb_led_set_color(RGB_LED_YELLOW);
                    batt_blink_iters = BATTERY_LOW_BLINK_ON_ITERS;
                } else {
                    rgb_led_off(); /* end of blink -> back to NORMAL idle (off) */
                    batt_blink_iters = BATTERY_LOW_BLINK_OFF_ITERS;
                }
            }
        } else {
            /*
             * Not eligible to blink (battery ok, or the LED is owned by an
             * active alert / green flash). If we were mid-blink and the LED is
             * ours to release (idle NORMAL, no green flash), turn it off so a
             * recovered battery leaves the LED off; otherwise leave the LED to
             * whoever owns it. Re-arm so the next eligible iteration starts
             * cleanly with an ON pulse.
             */
            if (batt_blink_on && alert_phase == ALERT_NORMAL &&
                green_flash_iters == 0) {
                rgb_led_off();
            }
            batt_blink_on = false;
            batt_blink_iters = 0;
        }

        err = mpu6050_read(&raw, &scaled);
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "%d,%d,%d,%d,%d,%d,"
                     "%.4f,%.4f,%.4f,%.2f,%.2f,%.2f",
                     raw.accel_x, raw.accel_y, raw.accel_z,
                     raw.gyro_x, raw.gyro_y, raw.gyro_z,
                     scaled.accel_x, scaled.accel_y, scaled.accel_z,
                     scaled.gyro_x, scaled.gyro_y, scaled.gyro_z);

            /* Feed this sample into the fall-detection state machine. The
             * call is cheap and non-blocking. It returns true only on the
             * single sample that newly transitions to SUSPECTED. */
            bool fall_suspected = fall_detector_update(
                scaled.accel_x, scaled.accel_y, scaled.accel_z,
                scaled.gyro_x, scaled.gyro_y, scaled.gyro_z);
            if (fall_suspected && alert_phase == ALERT_NORMAL) {
                ESP_LOGE(TAG, "*** FALL SUSPECTED *** (state machine: %s)",
                         fall_detector_state_name(fall_detector_get_state()));
                /* #FW4: enter the user-cancel COUNTDOWN. The user now has
                 * FALL_CANCEL_WINDOW_MS to SHORT-press cancel before the fall
                 * is treated as CONFIRMED (handled in the phase switch above
                 * on subsequent iterations). */
                alert_phase = ALERT_COUNTDOWN;
                /* #P3-USERCHECK-FW: tell any subscribed app a fall is suspected
                 * (Fall-Event -> 2) so it can show an "Are you OK?" prompt and
                 * optionally cancel via the Command char before we escalate to
                 * CONFIRMED. Non-blocking, no-op if BLE is down/unsubscribed. */
                ble_service_notify_fall_suspected();
                countdown_iters = FALL_CANCEL_WINDOW_ITERS;
                blink_iters = FALL_COUNTDOWN_BLINK_ITERS;
                blink_on = true;
                /* Start the long "about to alert" buzz ON immediately and seed
                 * the ON-duration counter; the ALERT_COUNTDOWN case toggles it
                 * non-blocking from here on. */
                countdown_vibe_on = true;
                countdown_vibe_iters = FALL_COUNTDOWN_VIBE_ON_ITERS;
                vibration_on();
                rgb_led_set_color(RGB_LED_RED); /* start of the blink cycle */
                green_flash_iters = 0; /* don't let a stale flash clear it */
            }
        } else {
            ESP_LOGW(TAG, "IMU read failed: %s", esp_err_to_name(err));
        }

        /* Low-rate battery log: gated behind a counter so it does not run at
         * the 50 Hz IMU rate. */
        if (battery_ok && --battery_log_iters <= 0) {
            battery_log_iters = FALLSENA_BATTERY_LOG_ITERS;
            int mv, pct;
            if (battery_read_mv(&mv) == ESP_OK &&
                battery_read_percent(&pct) == ESP_OK) {
                ESP_LOGI(TAG, "battery: %d mV (%d%%)", mv, pct);
                /* #FW5: push battery % + charging state to the app. charging
                 * is reported false: no charging-detect GPIO in the BOM (see
                 * ble_service_update_status / battery.h). Non-blocking. */
                ble_service_update_status(pct, false);

                /*
                 * #FW-BATTLOW: update the hysteretic, debounced low-battery
                 * flag. When NOT low, a read qualifies if pct <= ENTER; when
                 * low, it qualifies if pct >= EXIT. The flag only flips after
                 * BATTERY_LOW_DEBOUNCE_READS consecutive qualifying reads
                 * (~5 s apart), so a single noisy sample can't toggle it. The
                 * LED cue itself is driven non-blocking from the loop below.
                 */
                bool batt_qualifies = battery_low
                        ? (pct >= BATTERY_LOW_EXIT_PCT)
                        : (pct <= BATTERY_LOW_ENTER_PCT);
                if (batt_qualifies) {
                    if (++batt_low_streak >= BATTERY_LOW_DEBOUNCE_READS) {
                        battery_low = !battery_low;
                        batt_low_streak = 0;
                        ESP_LOGW(TAG, "battery %s (%d%%)",
                                 battery_low ? "LOW" : "recovered", pct);
                    }
                } else {
                    batt_low_streak = 0;
                }

                /*
                 * #FW-BATTPROT protective over-discharge cutoff. Track a
                 * critical-read streak on the TRUE terminal voltage (mV), not
                 * the %-curve. Once the cell has been critical for
                 * BATTERY_CRITICAL_DEBOUNCE_READS consecutive reads (~15 s) we
                 * shut down to stop draining it -- but ONLY while idle
                 * (ALERT_NORMAL). GATING: an active fall alert
                 * (COUNTDOWN/CONFIRMED) always outranks battery protection, so
                 * we never deep-sleep mid-alarm; the streak keeps counting so
                 * the cutoff fires as soon as the alert clears if still
                 * critical. Any non-critical read resets the streak.
                 */
                if (mv <= BATTERY_CRITICAL_MV) {
                    if (++batt_crit_streak >= BATTERY_CRITICAL_DEBOUNCE_READS &&
                        alert_phase == ALERT_NORMAL) {
                        battery_enter_protective_sleep(mv); /* no return */
                    }
                } else {
                    batt_crit_streak = 0;
                }
            } else {
                ESP_LOGW(TAG, "battery read failed");
            }
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(FALLSENA_SAMPLE_PERIOD_MS));
    }
}
