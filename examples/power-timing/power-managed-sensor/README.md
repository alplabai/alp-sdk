# power-managed-sensor

The SDK's reference for the v0.6 `cores.<id>.power:` declarative
block, on the AEN301 M55-HE (high-efficiency) core: a worked
example of sleep-mode selection + multi-source wakeup.

> **Scope: this example is a `board.yaml` reference, not a working
> sensor node.** `src/main.c` opens no sensor, no I2C, and no
> counter, and it never enters a sleep state -- `board.yaml`
> declares no `chips:` block at all. The "sample acquired" and
> "re-entering deep sleep" lines below are literal `printf` string
> constants that frame where a real node's `alp_*` calls would go.
> What is real and worth copying is the **declarative half**: the
> `power:` block and the `CONFIG_PM` / `CONFIG_PM_DEVICE` lines the planner
> emits from it. Despite the example's name, no BME280 or IMU is driven
> here (the SDK does ship a BME280 driver -- see
> [`<alp/chips/bme280.h>`](../../../include/alp/chips/bme280.h) --
> this example simply does not use it).

## What this shows

The duty cycle for a real low-power sensor node is "wake briefly,
sample, push, go back to sleep." Getting from milliamps to
microamps is the difference between weeks and years of battery
life -- which means the sleep state, the wake sources, and the
sample cadence all need to be first-class declarative facts, not
hand-tuned `CONFIG_PM_*` knobs scattered across `prj.conf`.

That declarative path is what this example demonstrates
end-to-end; the duty cycle itself is narrated, not executed.

The relevant fragment of `board.yaml`:

```yaml
cores:
  m55_he:
    os: zephyr
    app: ./src
    power:
      sleep_mode: deep
      wakeup_sources:
        - uart
        - gpio_int
        - rtc
    memory:
      stack_kib: 4
      heap_kib:  16
```

The orchestrator (`scripts/alp_orchestrate/`)
turns the `power:` block into:

```
# Per-slice power-management profile (board.yaml cores.<id>.power:)
CONFIG_PM=y
CONFIG_PM_DEVICE=y
# Sleep target: deep
# wakeup source: uart (DT wakeup-source; + pm_device_wakeup_enable() not implemented)
# wakeup source: gpio_int (DT wakeup-source; + pm_device_wakeup_enable() not implemented)
# wakeup source: rtc (DT wakeup-source; + pm_device_wakeup_enable() not implemented)
```

into the slice's generated `alp.conf`. Only `CONFIG_PM` and
`CONFIG_PM_DEVICE` are real Kconfig lines; the sleep target and the wake
sources are documentation hints until the per-silicon devicetree and
runtime-enable plumbing lands in v0.7. The customer never hand-edits the
`CONFIG_PM*` lines; the policy lives in `board.yaml`.

## SoM choice: AEN301

AEN301 is the small AEN SKU -- lower static power than the
AEN801 used by the `production-deployment` flagship. The
M55-HE (high-efficiency) core is the right home for the
always-on sensor task; the M55-HP (high-performance) core stays
parked (`os: "off"`) unless burst compute is needed (e.g. a
local TinyML inference on a vibration frame).

## The wake-source matrix

Three orthogonal triggers, each a one-line declaration in the
`wakeup_sources:` array:

| Source     | Latency | Use case                                 |
| ---------- | ------- | ---------------------------------------- |
| `rtc`      | <100 us | Periodic sensor sample (every 60 s)      |
| `gpio_int` | <50 us  | IMU motion event / user button press     |
| `uart`     | <1 ms   | Diagnostic console (lab / field debug)   |

`rtc` is the steady-state baseline (most wakes). `gpio_int` is
the event-driven path: the IMU's motion-detect IRQ pulls the
device out of sleep when the user moves; the user button does
the same for one-shot maintenance actions. `uart` is the
back-door for field debugging -- typing in the console wakes
the device long enough to dump diagnostics.

The wake-source list lives in `board.yaml` rather than the app so the
declared policy sits next to the rest of the slice config. Today it
lands in `alp.conf` as hint comments only: marking a device
wakeup-capable needs the DT `wakeup-source;` property plus a runtime
`pm_device_wakeup_enable()` call, and that plumbing is not implemented.

## Memory tuning

The `memory:` block is sized for the production node this example
frames, not for the printf body it actually ships. The reasoning:
a sensor task is short-lived per wake (sample + push, then back to
sleep), so 4 KiB of main-thread stack covers the sensor_value
vector + the I2C transfer buffer with headroom, and 16 KiB of heap
covers the Zephyr sensor subsystem's per-channel state. Both
numbers are declarative -- a low-power profile doesn't want the
Zephyr default 8 KiB / 1 KiB layout (heap truncated, stack
underused).

## Build

From this directory, or from your own copy of it:

```bash
tan build
tan flash
```

board.yaml declares both cores (`m55_hp` is parked with `os: "off"`), so only `m55_he` builds an application image.

The generated `alp.conf` carries the `CONFIG_PM=y` /
`CONFIG_PM_DEVICE=y` lines and the sleep-target / wake-source hint
comments above -- so you can verify the declarative path. The runtime behaviour is
the same three printf stages as on native_sim: the example does not
sleep, so a current probe will show the active draw throughout,
not a duty-cycled average. Measuring real deep-sleep current
requires replacing the printf bodies with actual sensor + PM
calls, as described under "Customising the cadence" below.

### Inside an SDK checkout (twister / CI)

These commands only work from an alp-sdk checkout, because `scripts/gen_example_alp_conf.py` writes `generated/alp.conf` there.

```bash
# writes examples/power-timing/power-managed-sensor/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/power-timing/power-managed-sensor
west build -b native_sim/native/64 examples/power-timing/power-managed-sensor -- -DEXTRA_CONF_FILE=generated/alp.conf
west build -t run
```

Expected output (native_sim; real silicon prints the same three stages):

```
[pm] power-managed-sensor (AEN301 / M55-HE)
[pm] wake sources: rtc(60s) | gpio_int(IMU/user) | uart(console)
[pm] sleep policy: deep -- see board.yaml cores.m55_he.power:
[pm] stage 1: wake-source=rtc
[pm]   sample acquired, host channel push -> ok
[pm]   re-entering deep sleep
[pm] stage 2: wake-source=gpio_int
[pm]   sample acquired, host channel push -> ok
[pm]   re-entering deep sleep
[pm] stage 3: wake-source=uart
[pm]   sample acquired, host channel push -> ok
[pm]   re-entering deep sleep
[pm] done
```

## Customising the cadence

The RTC tick period is an app concern -- `board.yaml` says
"RTC is a wake source," not "wake every 60 s." Customers
shipping a different cadence edit the `RTC_TICK_S` macro in
`src/main.c` (and, in a production node, the alarm channel for
the platform's counter device -- this framing demo only prints
the cadence, it doesn't program a counter). The `power:` block
stays the same.

## Reference

- [`docs/board-config-features.md`](../../../docs/board-config-features.md)
  "Per-slice power-management profile" -- the schema reference.
- [`examples/connectivity/production-deployment`](../../connectivity/production-deployment/)
  -- the application-core counterpart (sleep_mode: standby with
  wake-on-network for the Mender poll thread).
- [`docs/portability.md`](../../../docs/portability.md) -- which
  parts of this example stay portable across SoM families
  (moving to another family changes the `som.sku:` line and the `cores:`
  block).
