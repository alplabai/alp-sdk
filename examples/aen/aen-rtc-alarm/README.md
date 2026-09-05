# aen-rtc-alarm

On-module **RV-3028-C7** real-time clock and **alarm interrupt** on the
E1M-AEN801 (Alif Ensemble E8, Cortex-M55-HE), over **upstream Zephyr's `rtc_*`
API** — `drivers/rtc/rtc_rv3028.c`, compatible `microcrystal,rv3028`,
`CONFIG_RTC_RV3028`. [ADR 0017](../../../docs/adr/0017-alp-sdk-over-the-vendor-sdk.md)
Tier-1: no alp-sdk driver and no vendored code in the path, just a devicetree
node plus this app.

```bash
scripts/bench/aen/build.sh examples/aen/aen-rtc-alarm
```

## Read this first — three module facts that will bite you

### 1. There is NO backup supply. The clock does not survive a power cycle.

`VBACKUP` (U21 pin 6) has **no supply fitted on this batch**: its only other net
members are **R4** and **R68**, both 0-ohm and both **DNP**. No coin cell, no
supercapacitor, no trickle source.

That is why the devicetree carries:

| Property | Value | Why |
| --- | --- | --- |
| `backup-switch-mode` | `"disabled"` | Binding-**required**. `"direct"`/`"level"` would arm a switchover to a rail that does not exist. |
| `trickle-resistor-ohms` | **omitted** | Enabling a trickle charger into an unpopulated `VBACKUP` would be wrong. |

**Consequence:** neither the time **nor a pending alarm** is retained across a
power cycle. Every cold boot starts from an unset clock — which is exactly why
this example **sets** the time at boot rather than reading it. Do not build a
feature on this RTC that implies persistence; wall-clock time across a power
cycle has to come from the network or the host on this batch.

### 2. `RTC_CLKOUT` never reaches the SoC.

U21 pin 1 goes **only** to the E1M edge connector, **E1 pin AH16**. Firmware on
this module cannot consume it. The binding's `clkout-frequency` is therefore a
choice made **for a carrier designer**, and it is deliberately omitted from the
overlay so the pin stays LOW.

### 3. `MODULE_STBY` / EVI is a carrier-driven input.

U21 pin 8 (EVI, the external event input) comes **from** the edge connector,
**E1 pin O2**, with **R43 = 100k to +1V8**. Carrier hardware asserts it; nothing
on this module can. An EVI-timestamp feature is a carrier integration, not
something this app can exercise.

## Wiring

**Bus — BRD_I2C is SoC I2C0, function C.** Netlist rows
`"BRD_I2C_SDA",U6,B8,"P7_0/SCP0_A"` and `"BRD_I2C_SCL",U6,B3,"P7_1/SCP1_A"`;
the AE822 DFP maps P7_0 alt-fn [4] = `I2C0_SDA_C` and P7_1 alt-fn [4] =
`I2C0_SCL_C`. So this is `i2c@49010000`, `snps,designware-i2c`,
`interrupts = <132 3>`, driven by upstream `i2c_dw`. The RV-3028 answers at
**0x52**.

Two pad settings each cost a wasted bench run and are non-negotiable:

- **`input-enable` on BOTH pins.** Alif's own DFP sets `PADCTRL_READ_ENABLE` on
  P7_0 *and* P7_1. With it on SDA only, `i2c_dw` cannot sense SCL and NACKs
  every address for a reason that looks electrical but is not.
- **`bias-pull-up`, not `bias-pull-down`.** `soc/alif/ensemble/pinctrl_soc.h`
  encodes pad bits 19:20 `[DSC]` as `0` = high-Z, `1` = pull-up, `2` =
  pull-down, `3` = bus-keeper. Do **not** copy the I2C2/EEPROM overlay's
  `bias-pull-down`: that is a pull-**down**, harmless there only because that
  segment has external carrier pull-ups (R137/R144). BRD_I2C has **no** external
  pull-up anywhere — it never reaches the edge connector, so no carrier can add
  one. `drive-open-drain` is deliberately not set, leaving bit 23 `[DRV]` = 0 =
  push-pull.

**Alarm line — `/INT` → P15_0 → LPGPIO bit 0 → IRQ 171.** U21 pin 2 drives net
`RTC_ALARM` to SoC pad U6 V2 = `P15_0_FLEX`, with **R98 = 100k to +1V8 fitted**
as the open-drain pull-up. **Active LOW.** P15 is the low-power GPIO island
(`lpgpio@42002000`, `snps,designware-gpio`, `ngpios = <8>`, one IRQ per bit from
171 to 178), so bit 0 is IRQ 171. No pinctrl group is needed: `PIN_P15_0__LPGPIO`
is `ALIF_PINMUX(15, 0, 0)` — function 0, the pad's reset default.

## What it does

1. **`rtc_set_time()` then `rtc_get_time()`**, printing both — the round trip is
   shown, not asserted.
2. **Waits 3 s and reads again.** A part that merely answers on I2C is not a
   running clock; only a second read separated in time proves the 32.768 kHz
   oscillator is oscillating. (Bench 2026-09-05 already confirmed it does: the
   Seconds register advanced `0x01` → `0x02`, and ID register `0x28` reads
   `0x44`.)
3. **Arms an alarm and waits for the callback**, with a **bounded 90 s timeout**
   so a dead interrupt line ends the run instead of hanging it.
4. **Disarms and clears the flag**, so a re-run behaves identically.
5. Prints one machine-greppable **`RESULT PASS` / `RESULT PARTIAL` /
   `RESULT FAIL`** line.

### Why the alarm is a minute out, not "a few seconds"

The RV-3028's alarm register file holds **MINUTES, HOURS and DATE only — there
is no seconds alarm**. Upstream reflects that exactly:
`rv3028_alarm_get_supported_fields()` returns
`MINUTE | HOUR | MONTHDAY`, and `rv3028_alarm_set_time()` rejects any other bit
with `-EINVAL`.

The example still gets a short wait by **choosing the start time**: it sets the
clock to `12:30:40`, then arms for minute `31` with a **MINUTE-only mask**, so
the alarm fires roughly 20 s later. No hardware capability is invented, and a
minutes-only match keeps the arming free of hour/day rollover arithmetic.

### Why clearing the flag matters

`/INT` is open-drain and stays pulled **LOW for as long as `STATUS.AF` is set**,
and the upstream driver configures the GPIO as `GPIO_INT_EDGE_TO_ACTIVE`. Leave
`AF` set and the line is **stuck asserted**: the next run's alarm produces no
falling edge, so no interrupt and no callback — a failure that looks like a
broken alarm line rather than an uncleared flag from the run before.

The teardown order in `main()` is deliberate:

1. `rtc_alarm_set_callback(NULL)` — clears `CONTROL2.AIE`, so nothing new is
   raised mid-teardown.
2. `rtc_alarm_set_time(mask = 0)` — sets the alarm-enable bits in all three
   alarm registers, disabling the match.
3. `rtc_alarm_is_pending()` — reads **and clears** `STATUS.AF`, releasing `/INT`
   back high.

The app also clears a stale `AF` **before** installing its callback, because
`rtc_alarm_set_callback()` submits the driver's work item immediately: a
leftover flag would otherwise fire the callback the instant we arm, and the app
would report `PASS` without the hardware ever having raised an interrupt.

## Reading the RESULT line

| Line | Meaning |
| --- | --- |
| `RESULT PASS` | Set/get round-tripped, the clock advanced, and the alarm interrupt arrived within the timeout. |
| `RESULT PARTIAL` | **The RTC itself is healthy** — set/get round-tripped and the clock advanced — but no alarm callback arrived. This is an **interrupt-path** fault, not an RTC fault. |
| `RESULT FAIL` | Either the device did not probe at all, an API call errored, or the RTC answered on I2C but the clock **did not advance** — i.e. the oscillator is not running. Suspect the crystal/part, not the bus: the successful set/get already proved the bus. |

`PARTIAL` exists specifically so a bench operator is not sent after the wrong
part. If you get it, check in this order:

1. **The port-15 register layout.** The AE822 DFP warns *"Note that
   `LPGPIO_CTRL_n` register has a different layout!"* for port 15. This is the
   **most likely cause** — IRQ 171 may not be reaching the core even though the
   pad mux is correct. It is a known DFP-level risk, **not** an RTC defect.
2. **A stale `STATUS.AF`** holding `/INT` low with no falling edge for
   `GPIO_INT_EDGE_TO_ACTIVE` (see above).
3. **R98** (100k to +1V8) actually fitted on `RTC_ALARM`.

## Layering note

Everything in `boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay` is
a **module** fact, not an application choice, and is on its way down one layer:
the board files under `zephyr/boards/alp/e1m_aen801_m55_he/` are gaining the
BRD_I2C bus and the on-module device nodes so every AEN801 app gets them for
free. Until that lands the overlay carries them; because devicetree merges by
node **path** and the content is identical to what the board layer emits, the
two compose harmlessly and this overlay can then shrink.

The overlay filename must be the **fully-qualified** board id with `/` → `_`.
Zephyr auto-applies `boards/<name>.overlay` only on an exact match against the
build's qualified board id — a bare `alp_e1m_aen801_m55_he.overlay` is silently
ignored, and the app then builds with no RTC node at all.

## Status

**BUILD-VERIFIED**, bench-run pending. The bus and the part are already
bench-proven on E1M-AEN801 2626-R2 (2026-09-05, Flow A, cold-cycle proven): the
RV-3028 ACKs at `0x52`, ID register `0x28` = `0x44`, and the seconds register
advanced — the oscillator runs. The **alarm interrupt path** (P15_0 → IRQ 171)
has **not** been exercised on silicon yet; that is what the `PARTIAL` verdict
and its port-15 note above exist for.
