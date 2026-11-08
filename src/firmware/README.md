# FallSena Firmware

Firmware for the FallSena fall-detection wearable.

## Target chip

- **ESP32-C6** (RISC-V, Wi-Fi 6 + BLE 5)
- Toolchain: **ESP-IDF** (v5.x or newer for ESP32-C6 support)

## Project status

Phase 1: raw-data logging plus user-peripheral drivers. `app_main()`
initializes the MPU6050 IMU over I2C and streams raw + scaled
accelerometer/gyroscope readings over serial at a fixed rate. It also drives
the three user-facing peripherals (RGB LED, vibration motor, button) and runs
a small demo polled inside the same 50 Hz loop. It also reads the LiPo battery
voltage / state-of-charge via the ADC and logs it at a low rate.

Phase 2 (#FW3): the **fall-detection state machine** (`fall_detector.c/.h`)
consumes every 50 Hz IMU sample and decides when a fall is *suspected* (see
[Fall detection](#fall-detection) below). The user-cancel countdown (#FW4),
BLE alerting (#FW5), and power management (#FW6) are implemented in later tasks.

## Hardware (confirmed)

- ESP32-C6 MCU
- MPU6050 accelerometer/gyroscope over I2C
- 1× 3-pin RGB LED
- Coin vibration motor
- Push button
- Voltage divider for battery sensing on a 3.7V / 1000mAh LiPo

### Pin assignments

All peripheral GPIOs (used and planned) are documented in
[`main/pins.h`](main/pins.h). The values are valid ESP32-C6 defaults chosen
for documentation; **the owner must confirm them against the actual wiring
before flashing to hardware.**

I2C bus (MPU6050) — currently used:

| Signal      | GPIO    | Notes                          |
|-------------|---------|--------------------------------|
| I2C SDA     | GPIO6   | internal pull-up enabled       |
| I2C SCL     | GPIO7   | internal pull-up enabled       |
| I2C port    | 0       | HP I2C controller              |
| Bus clock   | 400 kHz | fast mode                      |
| MPU6050 addr| 0x68    | AD0 low                        |

User peripherals — currently used (pins still owner-confirmable):

| Peripheral      | GPIO(s)            | Driver           | Notes                                   |
|-----------------|--------------------|------------------|-----------------------------------------|
| RGB LED (R/G/B) | GPIO18/19/20       | `rgb_led.c/.h`   | analog 3-pin, LEDC PWM, 1 channel/color |
| Vibration motor | GPIO10             | `vibration.c/.h` | GPIO on/off, active-high via MOSFET     |
| Push button     | GPIO9 (strapping)  | `button.c/.h`    | active-low, internal pull-up, debounced |
| Battery ADC     | GPIO3 (ADC1 ch3)   | `battery.c/.h`   | LiPo divider tap, ADC oneshot + cali    |
| Fall detection  | (none)             | `fall_detector.c/.h` | software-only, consumes the 50 Hz IMU stream |

### RGB LED

Plain 3-pin **analog** RGB LED (one GPIO per color, no addressable controller),
driven with the **LEDC** peripheral: one shared 8-bit / 5 kHz timer
(`LEDC_TIMER_0`, low-speed mode) plus three channels — `LEDC_CHANNEL_0/1/2`
for R/G/B. `rgb_led_set(r, g, b)` takes 0–255 per channel; `rgb_led_set_color()`
takes a named color enum (off/red/green/blue/yellow/cyan/magenta/white).

Wiring **polarity** is a compile-time switch `RGB_LED_COMMON_ANODE` in
`rgb_led.c`. Default is **common-cathode** (`0`: duty == brightness); flip that
one line to `1` for a common-anode LED (duty is inverted) since the physical
wiring is not confirmed.

### Vibration motor

Coin motor on GPIO10, simple on/off (`vibration_on()` / `vibration_off()`).
`vibration_pulse(ms)` vibrates for `ms` then stops — it **blocks** the calling
task via `vTaskDelay` for that duration.

### Button

Active-low button on GPIO9 with the internal pull-up enabled. The API is
**polling**: call `button_get_event()` periodically (the main loop calls it
once per 50 Hz iteration). It returns `BUTTON_EVENT_NONE`, `BUTTON_EVENT_SHORT`,
or `BUTTON_EVENT_LONG`. The threshold is **`BUTTON_LONG_PRESS_MS` = 800 ms**:
a hold of 800 ms or more fires `LONG` (as soon as the threshold is crossed); a
shorter press fires `SHORT` on release. Debounce window is 25 ms.

> **GPIO9 is an ESP32-C6 strapping pin** and must be high at boot. The internal
> pull-up keeps it high when released, so a normal (released) button is safe,
> but holding the button down across a reset forces download/boot mode. Owner
> to confirm wiring.

The `main.c` demo: a SHORT press flashes the LED green (~300 ms, non-blocking)
and fires a 150 ms vibration pulse; a LONG press latches the LED blue. The demo
is polled inside the IMU loop and does not change the 50 Hz streaming rate
(aside from the brief blocking vibration pulse on a short press).

### Battery monitoring

The 3.7V / 1000mAh LiPo is sensed through an on-board resistor voltage divider
on **GPIO3**, which on the ESP32-C6 is **ADC unit 1, channel 3**
(`ADC_UNIT_1` / `ADC_CHANNEL_3`). The driver (`battery.c/.h`) uses the ADC
**oneshot** driver with **12 dB attenuation** (`ADC_ATTEN_DB_12`, widest input
range) and enables **curve-fitting calibration** when supported; if calibration
init fails it falls back to an approximate raw->mV conversion (~3100 mV
full-scale over the default 12-bit range). Each read averages **12 samples** to
reduce noise.

- `battery_read_mv()` returns the **battery-terminal** voltage in mV — the
  measured pin voltage multiplied back up by the divider ratio.
- `battery_read_percent()` returns state-of-charge clamped to **0..100%**.

> **`BATTERY_DIVIDER_RATIO` (in `battery.c`) — OWNER MUST SET.** It defaults to
> **`2.0`** (a 2:1 divider of two equal resistors, halving the battery voltage).
> The actual resistor values are not confirmed; set
> `ratio = (R_top + R_bottom) / R_bottom` to match the real board.

The voltage->% mapping is a **linear approximation** between
**3.30V = 0%** (`BATTERY_EMPTY_MV`) and **4.20V = 100%** (`BATTERY_FULL_MV`),
clamped to 0..100. The real LiPo discharge curve is non-linear; these endpoints
are tunable in `battery.c`.

Battery mV + % are logged via `ESP_LOGI` (tag `fallsena`) at a **low cadence —
once every `FALLSENA_BATTERY_LOG_ITERS` (250) loop iterations ≈ every 5 s** at
50 Hz — so the 50 Hz IMU stream is not spammed. Battery init is best-effort: if
it fails the IMU keeps streaming.

### IMU log output

The MPU6050 is configured with the default ranges (accel ±2 g, gyro ±250 °/s)
and sampled at **50 Hz** (`FALLSENA_SAMPLE_RATE_HZ` in `main/main.c`).

Each sample is logged via `ESP_LOGI` (tag `fallsena`) as one CSV line:

```
ax,ay,az,gx,gy,gz,ax_g,ay_g,az_g,gx_dps,gy_dps,gz_dps
```

- `ax,ay,az,gx,gy,gz` — raw `int16` register values.
- `ax_g,ay_g,az_g` — acceleration in g (4 decimals).
- `gx_dps,gy_dps,gz_dps` — angular rate in deg/s (2 decimals).

If MPU6050 init or the WHO_AM_I check fails, the firmware logs the error and
idles instead of emitting garbage.

## Fall detection

The core algorithm (`main/fall_detector.c` / `.h`) is a **state machine** fed
one scaled IMU sample per 50 Hz loop iteration via
`fall_detector_update(ax_g, ay_g, az_g, gx_dps, gy_dps, gz_dps)`. It implements
the classic accel-magnitude fall signature: **free-fall → impact → stillness**.

Each sample it computes two magnitudes:

- acceleration magnitude `mag = sqrt(ax_g² + ay_g² + az_g²)` (g; ≈ 1.0 at rest)
- gyro magnitude `gyro = sqrt(gx² + gy² + gz²)` (deg/s; ≈ 0 at rest)

### State machine

```
IDLE ──mag < FREE_FALL_LOW_G──▶ FREE_FALL ──mag > IMPACT_HIGH_G──▶ IMPACT
  ▲                                 │ window expires (no impact)        │
  │                                 └──────────────────────────────────┤ (begins stillness)
  │                                                                     ▼
  ├───────── motion resumes (out of still band) ──────────────── POST_IMPACT
  │                                                                     │ stillness held
  │                                                                     ▼
  └──────────────── fall_detector_reset() ─────────────────────── SUSPECTED (latched)
```

1. **IDLE → FREE_FALL**: acceleration magnitude drops below `FREE_FALL_LOW_G`
   (body in free descent). Starts a window.
2. **FREE_FALL → IMPACT**: within `FREE_FALL_WINDOW_MS` the magnitude spikes
   above `IMPACT_HIGH_G` (hitting the ground). If the window expires with no
   spike → back to **IDLE**. (IMPACT immediately begins POST_IMPACT.)
3. **POST_IMPACT (stillness monitor)**: the magnitude must stay in the band
   `[STILLNESS_LOW_G, STILLNESS_HIGH_G]` **and** gyro magnitude must stay below
   `STILLNESS_GYRO_DPS` for `STILLNESS_WINDOW_MS`. Any sample outside that band
   (motion resumes — e.g. they sat down hard / got back up) → back to **IDLE**.
4. **SUSPECTED**: stillness held for the full window → fall suspected. Latched
   until `fall_detector_reset()` (which #FW4's cancel flow will call) re-arms it.

### Thresholds & windows (defaults)

All are `#define`s grouped at the top of `fall_detector.c`. **These defaults are
starting points and require real controlled-drop-test tuning;** they move to NVS
in #FW7.

| `#define`                  | Default | Units   | Meaning                                              |
|----------------------------|---------|---------|------------------------------------------------------|
| `FREE_FALL_LOW_G`          | `0.5`   | g       | accel magnitude below this = free-fall begins        |
| `FREE_FALL_WINDOW_MS`      | `300`   | ms      | max time from free-fall to impact (15 samples)       |
| `IMPACT_HIGH_G`            | `2.5`   | g       | accel magnitude above this = ground impact           |
| `STILLNESS_LOW_G`          | `0.8`   | g       | lower edge of the at-rest (≈1 g) stillness band      |
| `STILLNESS_HIGH_G`         | `1.2`   | g       | upper edge of the at-rest stillness band             |
| `STILLNESS_GYRO_DPS`       | `30`    | deg/s   | gyro magnitude must stay below this during stillness |
| `STILLNESS_WINDOW_MS`      | `1500`  | ms      | stillness duration to confirm a fall (75 samples)    |

### Timing

Timing is **sample-count based**, not wall-clock. Because the main loop calls
`fall_detector_update()` exactly once per fixed-rate 50 Hz iteration, N samples
== N × 20 ms. The millisecond windows above are converted to sample counts at
compile time via `SAMPLES_FROM_MS(ms) = round(ms × 50 / 1000)`. This keeps the
module deterministic and free of any timer dependency.

### Detection event (edge-triggered)

`fall_detector_update()` returns `true` **only on the single sample** that newly
transitions the machine into `SUSPECTED` — it does **not** keep returning true
while latched. This edge-trigger means the reaction fires exactly once per fall.

On that edge, `main.c`:

- logs a prominent `*** FALL SUSPECTED ***` line via `ESP_LOGE` (tag
  `fallsena`), and
- latches the **RGB LED red** (`rgb_led_set_color(RGB_LED_RED)`).

That is the whole reaction at this stage. **#FW4** will add the vibrate-then-
cancel countdown (calling `fall_detector_reset()` on cancel) and **#FW5** the
BLE notification. The normal 50 Hz CSV IMU logging continues unchanged
throughout.

## Build

```sh
idf.py set-target esp32c6
idf.py build
```

## Flash & monitor

```sh
idf.py -p <PORT> flash monitor
```

Replace `<PORT>` with your serial device (e.g. `/dev/ttyACM0` on Linux,
`/dev/cu.usbserial-*` on macOS, or `COMx` on Windows).
