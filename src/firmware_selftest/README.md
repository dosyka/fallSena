# FallSena Hardware Self-Test Firmware

A one-shot bring-up diagnostic for the FallSena wearable (ESP32-C6). Flash this
**before** the real application firmware the first time you power up a board, to
confirm each peripheral is physically wired and working.

It runs every peripheral test once, prints a clear per-part `PASS` / `FAIL` /
`CHECK` summary to the serial monitor, then **halts** (it does not reboot-loop),
so the final summary stays on screen.

## What it tests (in order)

1. **IMU (MPU6050)** — `mpu6050_init()` talks to the sensor over I2C and checks
   `WHO_AM_I` (the real connectivity check). Then it reads ~10 samples at rest
   and confirms the total acceleration magnitude is roughly 1 g.
   Result: `PASS` / `FAIL` (with the measured magnitude).
2. **RGB LED** — drives the LED solid RED, GREEN, BLUE (~1 s each), then off.
   The log says which color *should* be showing at each step, so you can catch
   swapped channels or wrong polarity. Observational only → `CHECK`.
3. **Vibration motor** — pulses the motor twice (~300 ms each). Observational/
   tactile → `CHECK`.
4. **Button (GPIO9)** — prompts you to press the button within 10 seconds and
   polls for a press edge. Validates the GPIO9 strapping-pin concern.
   Result: `PASS` (press detected) / `FAIL` (timeout).
5. **Battery** — reads battery voltage (mV) and state-of-charge (%) through the
   ADC + divider. `PASS` if a plausible voltage is read, else `FAIL`
   (check the divider/ADC wiring).

`PASS`/`FAIL` are electrically verified by the firmware. `CHECK` items you must
confirm yourself by watching the LED / feeling the motor.

## How to flash

ESP-IDF is installed at `/home/home/esp/esp-idf/` but is **not** on `PATH`, so
source its export script first:

```sh
. /home/home/esp/esp-idf/export.sh
cd src/firmware_selftest
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with your serial device (e.g. `/dev/ttyACM0` or
`/dev/ttyUSB0`). Exit the monitor with `Ctrl-]`.

To build only (no hardware needed):

```sh
. /home/home/esp/esp-idf/export.sh
cd src/firmware_selftest
idf.py build
```

## How to read the output

Each step prints a clearly labeled banner (`STEP 1: IMU ...`, etc.) followed by
its detail lines. When everything has run, look for the `SELF-TEST SUMMARY`
block near the end:

```
#                SELF-TEST SUMMARY               #
  1. IMU (MPU6050) ......... PASS   (|a|=1.001 g)
  2. RGB LED ............... CHECK  (eyeball the colors)
  3. Vibration motor ....... CHECK  (feel the 2 buzzes)
  4. Button (GPIO9) ........ PASS
  5. Battery ............... PASS   (3920 mV, 87%)
```

Any `FAIL` line includes a hint about what to check (wiring, pull-ups, divider,
etc.). After the summary the firmware idles forever — power-cycle or flash the
real firmware when done.
