# drone-autopilot

> ⚠️ **`[UNTESTED]` -- v0.5 paper-correct.** Compiles on
> `native_sim/native/64` + cross-compiles to `alif_e7_dk_rtss_hp`.
> **Do NOT fly this build.**  The PID gains, sensor scales,
> failsafe thresholds, and motor mapping are starting-point
> values that need bench tuning + ground-tied throttle tests
> before a real airframe goes near them.

Self-contained PID-stabilised quadcopter flight controller
running on an E1M-AEN module.  The full real-time control stack
in one app: IMU sample → attitude estimation → cascaded PID →
motor mixer → ESCs.

## What it shows

- **Three deterministic control loops** in cooperating priorities:
  - 1 kHz rate loop (highest priority)
  - 250 Hz attitude loop
  - 25 Hz navigation + battery + failsafe loop
- **SBUS RC receiver** decoded on a UART input.
- **Five-state flight mode machine**: `DISARMED` →
  `MANUAL` / `STABILISE` / `GPS-HOLD` / `RTH` / `FAILSAFE`.
- **Failsafe** triggers on RC link loss OR critical battery, cuts
  throttle, descends to land.
- **Four chips**, all driven through portable `<alp/*>` surfaces:
  - LSM6DSO -- 6-axis IMU
  - BMP390 -- barometer
  - NEO-M9N -- GNSS (for GPS-HOLD + RTH)
  - INA236 -- battery voltage / current
- **Two libraries**: `madgwick_ahrs` (attitude fusion), `pid`
  (control kernel).  Loader-emitted FPU + Helium binding on
  AEN; TMU CORDIC on V2N.
- **MAVLink v2 ground-station link** -- minimal in-tree stack
  (`src/mavlink.{c,h}`) talks to QGroundControl / Mission Planner
  over a SiK telemetry radio on UART2 @ 57600 baud.
  - Tx: `HEARTBEAT` (1 Hz), `ATTITUDE` (10 Hz), `GPS_RAW_INT` +
    `GLOBAL_POSITION_INT` (5 Hz), `BATTERY_STATUS` (1 Hz).
  - Rx: `COMMAND_LONG` for arm/disarm + mode change, `HEARTBEAT`
    for GCS link-loss detection.
  - Swap the in-tree stack for upstream `c_library_v2` when you
    need the full 200-message dialect.

## Hardware

- E1M-AEN701 SoM.
- E1M-EVK carrier (4× PWM out, 1× SBUS UART in, I²C0).
- External 4S LiPo battery wired through the carrier's INA236
  shunt for telemetry.
- Standard quadcopter frame, four BLHeli-compatible ESCs +
  brushless motors.
- FrSky/Futaba SBUS-capable RC receiver (with carrier-side
  inverter -- SBUS is inverted UART).

## Build

```
west build -b alif_e7_dk_rtss_hp examples/drone-autopilot
west flash
```

## Tuning

Tune in this order:
1. Bench (props off, frame tied down): verify motor numbering,
   spin direction, throttle response, IMU axes.
2. Tethered hover: tune rate-loop PID gains so the airframe
   doesn't oscillate.
3. Free hover: tune attitude-loop PID gains so stick centring
   produces level flight.
4. Open environment with GPS lock: tune altitude + position PID.

Gains live in `src/autopilot.c`.  For production, move them to a
Zephyr settings backend so they persist across reboots.

## Verification status

`[UNTESTED]` -- paper-correct only.  Real-flight bring-up is
multi-week work: HiL + tethered hover + outdoor flight +
ESC-burn-out failure characterisation + RC-link-loss
characterisation + battery-fail characterisation, all logged
against a documented test plan.  Without that evidence
trail, **do not fly**.
