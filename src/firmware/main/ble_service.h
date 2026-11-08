/*
 * ble_service.h - BLE GATT server for the FallSena wearable (#FW5).
 *
 * Gives the caretaker's phone app a BLE surface to connect to. Built on the
 * NimBLE host stack (low footprint, standard for the ESP32-C6, BLE 5.0). The
 * NimBLE host runs in its own FreeRTOS task; every function here is safe to
 * call from the 50 Hz main loop whether or not a client is connected, and
 * none of them block on the radio.
 *
 * The device advertises connectably as "FallSena" and exposes two services:
 *
 * ---------------------------------------------------------------------------
 * 1. Standard Battery Service (SIG)
 *    Service UUID      : 0x180F
 *    Battery Level char: 0x2A19  (read + notify)
 *      Payload         : 1 byte, battery state-of-charge 0..100 (%)
 *
 * 2. Custom FallSena service
 *    Service UUID      : fa110000-5e5a-4fa1-1e00-000000000000
 *
 *    Fall-Event char   : fa110001-5e5a-4fa1-1e00-000000000000  (read + notify)
 *      Payload         : 1 byte
 *                          0 = none (no active suspected/confirmed fall)
 *                          1 = confirmed (a fall was CONFIRMED on-device)
 *                          2 = suspected (a fall is in the on-device cancel
 *                              grace window; the app may show an "Are you OK?"
 *                              prompt and write the cancel command below)
 *      A notification with value 2 is pushed the instant a fall enters the
 *      grace window, then 1 if it escalates to CONFIRMED, or 0 if cancelled.
 *
 *    Device-Status char: fa110002-5e5a-4fa1-1e00-000000000000  (read + notify)
 *      Payload         : 2 bytes, little-endian order:
 *                          byte[0] = battery percent, 0..100
 *                          byte[1] = charging flag, 0 = not charging/unknown,
 *                                    1 = charging
 *      NOTE: charging is currently always reported 0 (see ble_service.c) --
 *      there is no charging-detect GPIO in the BOM yet.
 *
 *    Command char      : fa110003-5e5a-4fa1-1e00-000000000000  (write)
 *      Payload         : 1 byte, opcode written by the app (device <- app)
 *                          0x01 = cancel the active fall grace countdown
 *                                 (identical effect to the on-device SHORT
 *                                 button press during the countdown)
 *      Other/unknown opcodes are ignored. Only meaningful while Fall-Event
 *      reads 2 (suspected); a cancel at any other time is a no-op.
 *
 * All 128-bit UUIDs are transmitted little-endian on-air per the BLE spec;
 * the strings above are the canonical (big-endian) form the app should use.
 * ---------------------------------------------------------------------------
 */

#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize NVS (used by the BLE stack for PHY calibration), bring up the
 * NimBLE host + controller, register the GATT services, set the device name
 * to "FallSena", and start connectable advertising. The NimBLE host runs in
 * its own task after this returns. Returns ESP_OK on success; on failure the
 * caller may log and continue (BLE is best-effort, the fall detector is core).
 */
esp_err_t ble_service_init(void);

/*
 * Set the Fall-Event characteristic to 1 (confirmed) and notify any subscribed
 * client. No-op (safe) if BLE is not up or nobody is subscribed. Call this at
 * the CONFIRMED transition in the main loop.
 */
void ble_service_notify_fall_confirmed(void);

/*
 * Set the Fall-Event characteristic to 2 (suspected) and notify any subscribed
 * client. No-op (safe) if BLE is not up or nobody is subscribed. Call this on
 * entering the on-device cancel grace window so the app can show an "Are you
 * OK?" prompt and (optionally) write the cancel command before the fall
 * escalates to CONFIRMED.
 */
void ble_service_notify_fall_suspected(void);

/*
 * Set the Fall-Event characteristic back to 0 (none) and notify any subscribed
 * client. No-op (safe) if BLE is not up or nobody is subscribed. Call this when
 * a SUSPECTED countdown is cancelled or a CONFIRMED alert is cleared so the app
 * re-arms for the next fall (the app reports a fall only on a rising edge, so
 * the flag must be reset to 0).
 */
void ble_service_notify_fall_cleared(void);

/*
 * Read-and-clear the app-requested cancel flag. Returns true exactly once if
 * the app has written FALLSENA_CMD_CANCEL (0x01) to the Command characteristic
 * since the last poll, then clears the flag; false otherwise. Poll this from
 * the main loop during the grace countdown to let the app cancel a suspected
 * fall, mirroring the on-device SHORT button press.
 */
bool ble_service_poll_app_cancel(void);

/*
 * Update the Battery Level and Device-Status characteristic values with the
 * latest battery percent (0..100) and charging flag, and notify subscribers.
 * No-op (safe) if BLE is not up or nobody is subscribed.
 */
void ble_service_update_status(int battery_pct, bool charging);

#ifdef __cplusplus
}
#endif
