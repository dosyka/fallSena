/*
 * fall_detector.c - Magnitude-only impact fall detector.
 *
 * See fall_detector.h for the public interface and full rationale.
 *
 * Per sample we compute the acceleration magnitude
 *     mag = sqrt(ax_g^2 + ay_g^2 + az_g^2)   (g; ~1.0 at rest)
 * and trigger on an impact spike alone. There is no gyro use, no tilt/
 * orientation math, no gravity baseline, and no post-impact stillness check.
 *
 * WHY this replaces the earlier two-stage (impact -> orientation-confirm)
 * design (owner decision):
 *   - the gyro was noisy during real falls and tended to VETO genuine events;
 *   - a stillness gate misses a conscious person struggling to get back up;
 *   - real impacts land at ~7-12 g, so a 3 g arm threshold is ample.
 * Accepted residual risk: an off-body dropped device can false-trigger; this is
 * mitigated downstream by the #FW4 user-cancel countdown.
 *
 *   IDLE:
 *     NOISE GUARD - mag must stay >= IMPACT_ARM_G on IMPACT_MIN_SAMPLES
 *     consecutive samples before we declare a fall. A single over-threshold
 *     sample must not trigger; any sample below threshold resets the counter.
 *     On the sample that reaches IMPACT_MIN_SAMPLES consecutive over-threshold
 *     samples we transition to SUSPECTED.
 *
 *   SUSPECTED:
 *     latched until fall_detector_reset() (e.g. the #FW4 cancel/clear flow).
 *
 * TIMING: sample-count based. The caller drives update() once per fixed-rate
 * 50 Hz loop iteration, so IMPACT_MIN_SAMPLES = 2 is ~40 ms.
 *
 * NOTE: the threshold and sample-count guard below are starting-point defaults.
 * In #FW7 these move into NVS so they can be tuned without reflashing.
 */

#include "fall_detector.h"

#include <math.h>
#include <stdint.h>

#include "esp_log.h"

/* ---- Sample rate this module assumes (must match the main 50 Hz loop) ---- */
#define FALL_SAMPLE_RATE_HZ        50

/* =========================================================================
 *  TUNABLE THRESHOLDS  (defaults from real drop-test data;
 *  to be moved to NVS in #FW7)
 * ========================================================================= */

/* Impact arm: accel magnitude at/above which a sample counts toward an impact.
 *     Units: g. */
#define IMPACT_ARM_G               3.0f

/* Noise guard: number of CONSECUTIVE samples at/above IMPACT_ARM_G required to
 *     declare a fall. At 50 Hz, 2 samples is ~40 ms; this rejects single-sample
 *     spikes. Units: samples. */
#define IMPACT_MIN_SAMPLES         2

static const char *TAG = "fall";

/* ---- State machine state ---- */
static fall_state_t s_state;

/* Count of consecutive samples so far with mag >= IMPACT_ARM_G (IDLE only). */
static uint32_t s_impact_run;

static inline float vec_magnitude(float x, float y, float z)
{
    return sqrtf(x * x + y * y + z * z);
}

void fall_detector_init(void)
{
    fall_detector_reset();
    ESP_LOGI(TAG,
             "fall detector ready @%dHz | magnitude-only: arm>=%.1fg on %d "
             "consecutive samples (~%dms)",
             FALL_SAMPLE_RATE_HZ, IMPACT_ARM_G, IMPACT_MIN_SAMPLES,
             (IMPACT_MIN_SAMPLES * 1000) / FALL_SAMPLE_RATE_HZ);
}

void fall_detector_reset(void)
{
    s_state = FALL_STATE_IDLE;
    s_impact_run = 0;
}

bool fall_detector_update(float ax_g, float ay_g, float az_g,
                          float gx_dps, float gy_dps, float gz_dps)
{
    /* Gyro is unused by the magnitude-only algorithm; kept in the signature so
     * the main.c call site does not change. */
    (void)gx_dps;
    (void)gy_dps;
    (void)gz_dps;

    const float mag = vec_magnitude(ax_g, ay_g, az_g);

    bool new_suspected = false;

    switch (s_state) {
    case FALL_STATE_IDLE:
        if (mag >= IMPACT_ARM_G) {
            /* Over threshold: advance the consecutive-sample noise guard. */
            s_impact_run++;
            if (s_impact_run >= IMPACT_MIN_SAMPLES) {
                s_state = FALL_STATE_SUSPECTED;
                new_suspected = true;
                ESP_LOGD(TAG, "impact confirmed (mag=%.2fg, %u consecutive)",
                         mag, (unsigned)s_impact_run);
            }
        } else {
            /* Below threshold: reset the consecutive counter. */
            s_impact_run = 0;
        }
        break;

    case FALL_STATE_SUSPECTED:
        /* Latched. Stays here until fall_detector_reset() (e.g. #FW4 cancel). */
        break;

    default:
        fall_detector_reset();
        break;
    }

    return new_suspected;
}

fall_state_t fall_detector_get_state(void)
{
    return s_state;
}

const char *fall_detector_state_name(fall_state_t state)
{
    switch (state) {
    case FALL_STATE_IDLE:       return "IDLE";
    case FALL_STATE_SUSPECTED:  return "SUSPECTED";
    default:                    return "UNKNOWN";
    }
}
