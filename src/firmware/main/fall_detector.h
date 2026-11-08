/*
 * fall_detector.h - Magnitude-only fall-detection state machine for FallSena.
 *
 * Consumes the scaled accelerometer samples produced by the MPU6050 driver at a
 * fixed 50 Hz rate (one sample per main-loop iteration) and decides when a fall
 * is SUSPECTED.
 *
 * MODEL (impact-magnitude only). The detector triggers on an acceleration-
 * magnitude impact spike alone: no gyro, no tilt/orientation, no gravity
 * baseline, no post-impact stillness check.
 *
 * WHY (owner decision, reversing the earlier two-stage impact -> orientation-
 * confirm design):
 *   - The gyro proved noisy during a real fall and tended to VETO genuine
 *     events rather than corroborate them.
 *   - A post-impact stillness gate misses a person who is conscious and
 *     struggling to get back up (they are not still).
 *   - Real impacts land around 7-12 g in the drop-test data, so a 3 g arm
 *     threshold is ample headroom to catch them while rejecting normal motion.
 * The orientation/gyro/stillness machinery was therefore removed entirely.
 *
 * ACCEPTED RESIDUAL RISK: a device that is dropped while off-body can produce a
 * magnitude spike and false-trigger. This is accepted and mitigated downstream
 * by the #FW4 user-cancel countdown, which lets the wearer dismiss a false
 * alarm before it escalates.
 *
 *   IDLE
 *     - per sample compute mag = sqrt(ax_g^2 + ay_g^2 + az_g^2) (g)
 *     - NOISE GUARD: mag must stay >= IMPACT_ARM_G on IMPACT_MIN_SAMPLES
 *       consecutive samples; a single over-threshold sample does NOT trigger.
 *       A sample below threshold resets the consecutive counter.
 *     - on the sample that reaches IMPACT_MIN_SAMPLES consecutive over-threshold
 *       samples -> SUSPECTED
 *
 *   SUSPECTED  (latched until fall_detector_reset())
 *
 * Detection is edge-triggered: fall_detector_update() returns true on the ONE
 * sample where the machine transitions into SUSPECTED, not on every sample
 * while it stays SUSPECTED.
 *
 * Timing is tracked using SAMPLE COUNTS at the known 50 Hz rate rather than
 * wall-clock time: the caller invokes update() once per 50 Hz loop iteration,
 * so N samples == N * 20 ms. This keeps the module free of any timer dependency
 * and deterministic against the fixed-rate loop.
 *
 * The gyro parameters remain in fall_detector_update()'s signature so the call
 * site in main.c is unchanged, but they are unused by the algorithm.
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Fall-detection state machine states. */
typedef enum {
    FALL_STATE_IDLE = 0,    /* normal activity; watching for an impact spike */
    FALL_STATE_SUSPECTED,   /* impact spike confirmed -> fall suspected      */
} fall_state_t;

/*
 * Initialize / reset the detector to IDLE. Call once before the first
 * fall_detector_update(). Safe to call again to re-arm.
 */
void fall_detector_init(void);

/*
 * Force the state machine back to IDLE, clearing any in-progress detection.
 * Called by #FW4's user-cancel flow to dismiss a false alarm, and also clears a
 * latched SUSPECTED state so detection can re-arm.
 */
void fall_detector_reset(void);

/*
 * Feed one 50 Hz sample (scaled accel in g, gyro in deg/s) into the state
 * machine. Call exactly once per IMU sample. The gyro arguments are accepted
 * for call-site compatibility but are unused by the magnitude-only algorithm.
 *
 * Returns true ONLY on the sample that newly transitions the machine into
 * FALL_STATE_SUSPECTED (edge-triggered: fires once per fall). Returns false
 * otherwise, including on subsequent samples while still SUSPECTED.
 */
bool fall_detector_update(float ax_g, float ay_g, float az_g,
                          float gx_dps, float gy_dps, float gz_dps);

/* Current state machine state (for logging / debug). */
fall_state_t fall_detector_get_state(void);

/* Human-readable name for a state (for logging). */
const char *fall_detector_state_name(fall_state_t state);

#ifdef __cplusplus
}
#endif
