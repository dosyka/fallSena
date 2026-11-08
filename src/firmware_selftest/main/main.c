/*
 * FallSena hardware bring-up / self-test firmware (ESP32-C6).
 *
 * PURPOSE
 * -------
 * This is a ONE-SHOT diagnostic to flash BEFORE the real firmware, the very
 * first time the board is powered on. It walks every peripheral in sequence
 * and prints a clear PASS / FAIL / CHECK result for each, so the owner can
 * confirm that each part is physically wired and working before trusting the
 * application firmware.
 *
 * It runs every test once, prints a tidy summary block, then HALTS (it does
 * NOT reboot-loop) so the final summary stays on screen in the serial monitor.
 *
 * Tests, in order:
 *   1. IMU (MPU6050) over I2C  -> PASS/FAIL  (WHO_AM_I + ~1 g rest magnitude)
 *   2. RGB LED                 -> VISUAL CHECK (red, green, blue, off)
 *   3. Vibration motor         -> VISUAL/TACTILE CHECK (two pulses)
 *   4. Button (GPIO9 strap)    -> PASS/FAIL  (press detected within 10 s)
 *   5. Battery ADC + divider   -> PASS/FAIL  (plausible mV read)
 *
 * Because this output is what the owner reads to debug wiring, every step is
 * heavily and explicitly labeled.
 */

#include <math.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include "mpu6050.h"
#include "rgb_led.h"
#include "vibration.h"
#include "button.h"
#include "battery.h"

static const char *TAG = "selftest";

/* --- Test tuning constants ------------------------------------------------ */

/* IMU: how many samples to average for the at-rest gravity-magnitude check. */
#define IMU_SAMPLE_COUNT        10
/* IMU: accepted total-acceleration magnitude window at rest, in g.
 * At rest the only acceleration is gravity (~1 g). A wide window keeps this
 * a sanity check (is the sensor producing real, sane numbers) rather than a
 * calibration test. */
#define IMU_MAG_MIN_G           0.5f
#define IMU_MAG_MAX_G           1.5f

/* RGB LED: how long to hold each color so the owner can eyeball it (ms). */
#define LED_COLOR_HOLD_MS       1000

/* Vibration: pulse length and the gap between the two pulses (ms). */
#define VIB_PULSE_MS            300
#define VIB_GAP_MS              300

/* Button: how long to wait for the owner to press it (ms) and poll period. */
#define BUTTON_WAIT_MS          10000
#define BUTTON_POLL_MS          20

/* Battery: minimum plausible reading. Anything at or below this almost
 * certainly means the divider / wiring is wrong, not a flat battery. */
#define BATTERY_MIN_PLAUSIBLE_MV 100

/* --- Per-test result tracking -------------------------------------------- */

typedef enum {
    RESULT_PASS = 0,   /* electrically verified OK */
    RESULT_FAIL,       /* electrically verified bad */
    RESULT_CHECK,      /* observational only (owner must eyeball it) */
} test_result_t;

static const char *result_str(test_result_t r)
{
    switch (r) {
    case RESULT_PASS:  return "PASS";
    case RESULT_FAIL:  return "FAIL";
    case RESULT_CHECK: return "CHECK";
    default:           return "????";
    }
}

/* --- Small helpers -------------------------------------------------------- */

/* Print a prominent step header so each section is easy to find in the log. */
static void print_step(int n, const char *title)
{
    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "==================================================");
    ESP_LOGI(TAG, "  STEP %d: %s", n, title);
    ESP_LOGI(TAG, "==================================================");
}

/* ========================================================================== */
/*  TEST 1: IMU (MPU6050)                                                      */
/* ========================================================================== */
/*
 * The real connectivity check is inside mpu6050_init(): it talks to the sensor
 * over I2C and verifies WHO_AM_I (reg 0x75 == 0x68). If that passes, the I2C
 * bus is wired and the chip is alive. We then take a handful of samples at rest
 * and confirm the total acceleration magnitude is roughly 1 g, which proves the
 * data path actually returns sane physical values.
 */
static test_result_t test_imu(float *out_mag_g)
{
    *out_mag_g = 0.0f;

    ESP_LOGI(TAG, "Initializing MPU6050 over I2C (checks WHO_AM_I)...");
    esp_err_t err = mpu6050_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "  -> mpu6050_init FAILED: %s", esp_err_to_name(err));
        ESP_LOGE(TAG, "     Check: SDA/SCL wiring, pull-ups, 3V3 power, address.");
        return RESULT_FAIL;
    }
    ESP_LOGI(TAG, "  -> WHO_AM_I OK, sensor is responding on I2C.");

    ESP_LOGI(TAG, "Reading %d samples at rest (board should be still)...",
             IMU_SAMPLE_COUNT);

    float mag_sum = 0.0f;
    int   good_samples = 0;
    for (int i = 0; i < IMU_SAMPLE_COUNT; i++) {
        mpu6050_scaled_data_t s;
        if (mpu6050_read(NULL, &s) != ESP_OK) {
            ESP_LOGW(TAG, "  sample %d: read failed", i);
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        float mag = sqrtf(s.accel_x * s.accel_x +
                          s.accel_y * s.accel_y +
                          s.accel_z * s.accel_z);
        ESP_LOGI(TAG,
                 "  sample %2d: ax=%+.3f ay=%+.3f az=%+.3f g | |a|=%.3f g",
                 i, s.accel_x, s.accel_y, s.accel_z, mag);
        mag_sum += mag;
        good_samples++;
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    if (good_samples == 0) {
        ESP_LOGE(TAG, "  -> No samples read back; data path is broken.");
        return RESULT_FAIL;
    }

    float mag_avg = mag_sum / good_samples;
    *out_mag_g = mag_avg;
    ESP_LOGI(TAG, "  -> average |a| at rest = %.3f g (expect ~1.0 g)", mag_avg);

    if (mag_avg >= IMU_MAG_MIN_G && mag_avg <= IMU_MAG_MAX_G) {
        ESP_LOGI(TAG, "  -> magnitude within [%.1f, %.1f] g: PASS",
                 IMU_MAG_MIN_G, IMU_MAG_MAX_G);
        return RESULT_PASS;
    }

    ESP_LOGE(TAG, "  -> magnitude OUTSIDE [%.1f, %.1f] g: FAIL "
                  "(scale/wiring/mounting?)", IMU_MAG_MIN_G, IMU_MAG_MAX_G);
    return RESULT_FAIL;
}

/* ========================================================================== */
/*  TEST 2: RGB LED                                                            */
/* ========================================================================== */
/*
 * Purely observational: drive each color channel solid for a second so the
 * owner can confirm the right channel lights with the right color (catches
 * swapped R/G/B wiring and common-anode vs common-cathode polarity mistakes).
 * There is no electrical pass/fail here -> result is always CHECK.
 */
static test_result_t test_rgb_led(void)
{
    if (rgb_led_init() != ESP_OK) {
        ESP_LOGE(TAG, "  -> rgb_led_init FAILED (LEDC setup). Marking CHECK.");
        return RESULT_CHECK;
    }

    ESP_LOGI(TAG, "The LED should now show, in order: RED, GREEN, BLUE, off.");
    ESP_LOGI(TAG, "Watch it and confirm each color matches its label.");

    ESP_LOGI(TAG, "  LED SHOULD BE: RED");
    rgb_led_set_color(RGB_LED_RED);
    vTaskDelay(pdMS_TO_TICKS(LED_COLOR_HOLD_MS));

    ESP_LOGI(TAG, "  LED SHOULD BE: GREEN");
    rgb_led_set_color(RGB_LED_GREEN);
    vTaskDelay(pdMS_TO_TICKS(LED_COLOR_HOLD_MS));

    ESP_LOGI(TAG, "  LED SHOULD BE: BLUE");
    rgb_led_set_color(RGB_LED_BLUE);
    vTaskDelay(pdMS_TO_TICKS(LED_COLOR_HOLD_MS));

    ESP_LOGI(TAG, "  LED SHOULD BE: OFF");
    rgb_led_off();

    ESP_LOGI(TAG, "  -> VISUAL CHECK: did the colors match the labels above?");
    return RESULT_CHECK;
}

/* ========================================================================== */
/*  TEST 3: Vibration motor                                                    */
/* ========================================================================== */
/*
 * Observational/tactile: buzz the motor twice. The owner should feel/hear two
 * distinct pulses. No electrical pass/fail -> result is always CHECK.
 */
static test_result_t test_vibration(void)
{
    if (vibration_init() != ESP_OK) {
        ESP_LOGE(TAG, "  -> vibration_init FAILED (GPIO setup). Marking CHECK.");
        return RESULT_CHECK;
    }

    ESP_LOGI(TAG, "Pulsing the vibration motor TWICE (%d ms each).", VIB_PULSE_MS);
    ESP_LOGI(TAG, "You should feel/hear two distinct buzzes.");

    ESP_LOGI(TAG, "  buzz 1/2 ...");
    vibration_pulse(VIB_PULSE_MS);     /* blocks for VIB_PULSE_MS */
    vTaskDelay(pdMS_TO_TICKS(VIB_GAP_MS));

    ESP_LOGI(TAG, "  buzz 2/2 ...");
    vibration_pulse(VIB_PULSE_MS);

    ESP_LOGI(TAG, "  -> VISUAL/TACTILE CHECK: did you feel two pulses?");
    return RESULT_CHECK;
}

/* ========================================================================== */
/*  TEST 4: Button (GPIO9 strapping pin)                                       */
/* ========================================================================== */
/*
 * GPIO9 on the ESP32-C6 is a strapping pin, so we must confirm it works as a
 * normal input once the board is running. Prompt the owner, then poll for a
 * press edge for up to BUTTON_WAIT_MS. PASS on any detected press, FAIL on
 * timeout.
 */
static test_result_t test_button(void)
{
    if (button_init() != ESP_OK) {
        ESP_LOGE(TAG, "  -> button_init FAILED (GPIO setup): FAIL");
        return RESULT_FAIL;
    }

    /* Drain any event already queued by the debounce state machine so a stale
     * press from a previous step can't auto-pass this test. */
    (void)button_get_event();

    ESP_LOGW(TAG, ">>> PLEASE PRESS THE BUTTON within %d seconds <<<",
             BUTTON_WAIT_MS / 1000);

    int waited_ms = 0;
    while (waited_ms < BUTTON_WAIT_MS) {
        button_event_t ev = button_get_event();
        if (ev == BUTTON_EVENT_SHORT || ev == BUTTON_EVENT_LONG) {
            ESP_LOGI(TAG, "  -> button press detected (%s): PASS",
                     ev == BUTTON_EVENT_LONG ? "LONG" : "SHORT");
            return RESULT_PASS;
        }
        vTaskDelay(pdMS_TO_TICKS(BUTTON_POLL_MS));
        waited_ms += BUTTON_POLL_MS;
    }

    ESP_LOGE(TAG, "  -> no press detected within %d s: FAIL",
             BUTTON_WAIT_MS / 1000);
    ESP_LOGE(TAG, "     Check: button wiring to GPIO9/GND, strapping conflict.");
    return RESULT_FAIL;
}

/* ========================================================================== */
/*  TEST 5: Battery ADC + voltage divider                                      */
/* ========================================================================== */
/*
 * Read the battery terminal voltage (mV) and state-of-charge (%) through the
 * on-board divider on the ADC. PASS if we get a plausible voltage (> a small
 * floor), otherwise FAIL -> almost always a divider/ADC wiring problem.
 */
static test_result_t test_battery(int *out_mv, int *out_pct)
{
    *out_mv = 0;
    *out_pct = 0;

    if (battery_init() != ESP_OK) {
        ESP_LOGE(TAG, "  -> battery_init FAILED (ADC setup): FAIL");
        return RESULT_FAIL;
    }

    int mv = 0, pct = 0;
    esp_err_t err_mv  = battery_read_mv(&mv);
    esp_err_t err_pct = battery_read_percent(&pct);

    if (err_mv != ESP_OK) {
        ESP_LOGE(TAG, "  -> battery_read_mv FAILED: %s", esp_err_to_name(err_mv));
        return RESULT_FAIL;
    }

    *out_mv = mv;
    *out_pct = (err_pct == ESP_OK) ? pct : -1;

    if (err_pct == ESP_OK) {
        ESP_LOGI(TAG, "  -> battery: %d mV (%d%%)", mv, pct);
    } else {
        ESP_LOGW(TAG, "  -> battery: %d mV (percent read failed: %s)",
                 mv, esp_err_to_name(err_pct));
    }

    if (mv > BATTERY_MIN_PLAUSIBLE_MV) {
        ESP_LOGI(TAG, "  -> voltage is plausible (> %d mV): PASS",
                 BATTERY_MIN_PLAUSIBLE_MV);
        return RESULT_PASS;
    }

    ESP_LOGE(TAG, "  -> voltage implausibly low (<= %d mV): FAIL "
                  "-- check the divider/ADC wiring.", BATTERY_MIN_PLAUSIBLE_MV);
    return RESULT_FAIL;
}

/* ========================================================================== */
/*  Entry point                                                               */
/* ========================================================================== */
void app_main(void)
{
    /* Give the serial monitor a moment to attach so the banner isn't missed. */
    vTaskDelay(pdMS_TO_TICKS(500));

    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "##################################################");
    ESP_LOGI(TAG, "#        FallSena HARDWARE SELF-TEST             #");
    ESP_LOGI(TAG, "#        (ESP32-C6 bring-up diagnostic)          #");
    ESP_LOGI(TAG, "##################################################");
    ESP_LOGI(TAG, "This runs each peripheral once and prints a");
    ESP_LOGI(TAG, "PASS / FAIL / CHECK summary at the end, then halts.");
    ESP_LOGI(TAG, "Have the board still for the IMU step, watch the LED,");
    ESP_LOGI(TAG, "feel the motor, and be ready to press the button.");

    /* Results, defaulting to FAIL so a crash mid-test never reads as PASS. */
    test_result_t r_imu     = RESULT_FAIL;
    test_result_t r_led     = RESULT_CHECK;
    test_result_t r_vib     = RESULT_CHECK;
    test_result_t r_button  = RESULT_FAIL;
    test_result_t r_battery = RESULT_FAIL;

    float imu_mag_g = 0.0f;
    int   batt_mv = 0, batt_pct = 0;

    print_step(1, "IMU (MPU6050) over I2C");
    r_imu = test_imu(&imu_mag_g);

    print_step(2, "RGB LED (visual)");
    r_led = test_rgb_led();

    print_step(3, "Vibration motor (tactile)");
    r_vib = test_vibration();

    print_step(4, "Button (GPIO9)");
    r_button = test_button();

    print_step(5, "Battery (ADC + divider)");
    r_battery = test_battery(&batt_mv, &batt_pct);

    /* --- Final summary block --------------------------------------------- */
    ESP_LOGI(TAG, " ");
    ESP_LOGI(TAG, "##################################################");
    ESP_LOGI(TAG, "#                SELF-TEST SUMMARY               #");
    ESP_LOGI(TAG, "##################################################");
    ESP_LOGI(TAG, "  1. IMU (MPU6050) ......... %-5s  (|a|=%.3f g)",
             result_str(r_imu), imu_mag_g);
    ESP_LOGI(TAG, "  2. RGB LED ............... %-5s  (eyeball the colors)",
             result_str(r_led));
    ESP_LOGI(TAG, "  3. Vibration motor ....... %-5s  (feel the 2 buzzes)",
             result_str(r_vib));
    ESP_LOGI(TAG, "  4. Button (GPIO9) ........ %-5s", result_str(r_button));
    ESP_LOGI(TAG, "  5. Battery ............... %-5s  (%d mV, %d%%)",
             result_str(r_battery), batt_mv, batt_pct);
    ESP_LOGI(TAG, "--------------------------------------------------");
    ESP_LOGI(TAG, "  PASS/FAIL = electrically verified.");
    ESP_LOGI(TAG, "  CHECK     = you must visually/physically confirm it.");
    ESP_LOGI(TAG, "##################################################");
    ESP_LOGI(TAG, "Self-test complete. Halting (no reboot loop).");

    /* Halt: idle forever so the summary stays on screen. Do NOT reboot. */
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}
