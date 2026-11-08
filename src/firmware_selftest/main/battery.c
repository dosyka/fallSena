/*
 * battery.c - LiPo battery voltage / state-of-charge monitor implementation.
 *
 * See battery.h for the public interface. Uses the ESP-IDF ADC oneshot driver
 * (esp_adc/adc_oneshot.h) plus curve-fitting calibration (esp_adc/adc_cali.h)
 * when supported, falling back to a raw->mV estimate when calibration init
 * fails or is unsupported.
 *
 * Pin mapping: FALLSENA_BATTERY_ADC_GPIO is GPIO3, which on the ESP32-C6 is
 * ADC unit 1, channel 3 (ADC1_CHANNEL_3). GPIO0..6 are the ESP32-C6 ADC1
 * channels; we assert the pin maps to a valid channel at init.
 */

#include "battery.h"
#include "pins.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

/*
 * Voltage divider ratio: V_battery = V_adc_pin * BATTERY_DIVIDER_RATIO.
 *
 * !!! OWNER ACTION REQUIRED !!!
 * Default assumes a 2:1 divider built from two EQUAL resistors (e.g. two 100k),
 * which halves the battery voltage so a full 4.2V cell reads ~2.1V at the pin
 * (within the 12 dB ADC range). The ACTUAL resistor values on the FallSena
 * board are NOT confirmed. The owner MUST set this ratio to match the real
 * resistors: ratio = (R_top + R_bottom) / R_bottom, where R_bottom is the
 * resistor across which the ADC measures (battery+ -> R_top -> tap -> R_bottom
 * -> GND, ADC reads the tap).
 */
#define BATTERY_DIVIDER_RATIO   2.0f

/* GPIO3 on the ESP32-C6 is ADC unit 1, channel 3. */
#define BATTERY_ADC_UNIT        ADC_UNIT_1
#define BATTERY_ADC_CHANNEL     ADC_CHANNEL_3

/*
 * 12 dB attenuation gives the widest usable input range (~0..~3.1-3.3V on the
 * ESP32-C6), which comfortably covers a halved LiPo voltage (~1.65..2.1V).
 */
#define BATTERY_ADC_ATTEN       ADC_ATTEN_DB_12

/* Number of ADC samples averaged per read to reduce noise. */
#define BATTERY_SAMPLE_COUNT    12

/*
 * Approximate LiPo voltage->% endpoints, in millivolts at the battery terminal.
 * This is a simple LINEAR approximation between empty and full and is only a
 * rough guide; the real discharge curve is non-linear. Tunable by the owner.
 *   ~3.30V -> 0%   (conservative empty cutoff for a 3.7V LiPo)
 *   ~4.20V -> 100% (fully charged)
 */
#define BATTERY_EMPTY_MV        3300
#define BATTERY_FULL_MV         4200

static const char *TAG = "battery";

static adc_oneshot_unit_handle_t s_adc_handle;
static adc_cali_handle_t s_cali_handle;   /* NULL if calibration unavailable */
static bool s_calibrated;
static bool s_initialized;

/* ------------------------------------------------------------------------- */
/* Calibration helper                                                        */
/* ------------------------------------------------------------------------- */

/* Try to create a calibration scheme for this unit/channel/atten. Returns
 * true and sets *out on success; false if calibration is unsupported/fails. */
static bool battery_cali_init(adc_cali_handle_t *out)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .chan     = BATTERY_ADC_CHANNEL,
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_curve_fitting(&cfg, out);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: curve fitting");
        return true;
    }
    ESP_LOGW(TAG, "curve-fitting cali failed (%s); using raw conversion",
             esp_err_to_name(err));
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id  = BATTERY_ADC_UNIT,
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    esp_err_t err = adc_cali_create_scheme_line_fitting(&cfg, out);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "ADC calibration: line fitting");
        return true;
    }
    ESP_LOGW(TAG, "line-fitting cali failed (%s); using raw conversion",
             esp_err_to_name(err));
#else
    ESP_LOGW(TAG, "no ADC calibration scheme supported; using raw conversion");
#endif
    *out = NULL;
    return false;
}

/* ------------------------------------------------------------------------- */
/* Public API                                                                */
/* ------------------------------------------------------------------------- */

esp_err_t battery_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }

    /* Sanity: this driver maps GPIO3 -> ADC1 ch3; if the pin is changed the
     * channel mapping above must be updated to match. */
    if (FALLSENA_BATTERY_ADC_GPIO != 3) {
        ESP_LOGW(TAG, "FALLSENA_BATTERY_ADC_GPIO=%d but driver is hard-mapped "
                      "to GPIO3 (ADC1 ch3); update battery.c if you change it",
                 FALLSENA_BATTERY_ADC_GPIO);
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = BATTERY_ADC_UNIT,
    };
    esp_err_t err = adc_oneshot_new_unit(&unit_cfg, &s_adc_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed: %s", esp_err_to_name(err));
        return err;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten    = BATTERY_ADC_ATTEN,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_oneshot_config_channel(s_adc_handle, BATTERY_ADC_CHANNEL,
                                     &chan_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed: %s",
                 esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        return err;
    }

    /* Calibration is best-effort; raw->mV fallback is used if it fails. */
    s_calibrated = battery_cali_init(&s_cali_handle);

    s_initialized = true;
    ESP_LOGI(TAG, "battery monitor ready (GPIO%d -> ADC1 ch%d, 12dB, "
                  "divider %.2f, cali=%s)",
             FALLSENA_BATTERY_ADC_GPIO, BATTERY_ADC_CHANNEL,
             BATTERY_DIVIDER_RATIO, s_calibrated ? "on" : "off");
    return ESP_OK;
}

esp_err_t battery_read_mv(int *out_mv)
{
    if (!s_initialized || s_adc_handle == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (out_mv == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    /* Average several raw samples to suppress noise. */
    int32_t raw_sum = 0;
    for (int i = 0; i < BATTERY_SAMPLE_COUNT; i++) {
        int raw;
        esp_err_t err = adc_oneshot_read(s_adc_handle, BATTERY_ADC_CHANNEL,
                                         &raw);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_oneshot_read failed: %s", esp_err_to_name(err));
            return err;
        }
        raw_sum += raw;
    }
    int raw_avg = (int)(raw_sum / BATTERY_SAMPLE_COUNT);

    int pin_mv;
    if (s_calibrated) {
        esp_err_t err = adc_cali_raw_to_voltage(s_cali_handle, raw_avg, &pin_mv);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "adc_cali_raw_to_voltage failed: %s",
                     esp_err_to_name(err));
            return err;
        }
    } else {
        /*
         * Uncalibrated fallback: assume a ~3100 mV full-scale at 12 dB over the
         * default 12-bit range (0..4095). This is approximate; calibration is
         * strongly preferred when available.
         */
        pin_mv = (int)((int64_t)raw_avg * 3100 / 4095);
    }

    /* Scale the measured pin voltage back up to the battery-terminal voltage. */
    *out_mv = (int)(pin_mv * BATTERY_DIVIDER_RATIO + 0.5f);
    return ESP_OK;
}

esp_err_t battery_read_percent(int *out_pct)
{
    if (out_pct == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    int mv;
    esp_err_t err = battery_read_mv(&mv);
    if (err != ESP_OK) {
        return err;
    }

    /* Linear voltage->% between the empty/full endpoints, clamped to 0..100.
     * NOTE: approximate; the real LiPo discharge curve is non-linear. */
    int pct = (int)(((int64_t)(mv - BATTERY_EMPTY_MV) * 100) /
                    (BATTERY_FULL_MV - BATTERY_EMPTY_MV));
    if (pct < 0) {
        pct = 0;
    } else if (pct > 100) {
        pct = 100;
    }

    *out_pct = pct;
    return ESP_OK;
}
