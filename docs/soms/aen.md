# E1M-AEN family

> Alif Ensemble-based SoMs in the E1M (35 × 35 mm) form factor.

## SKUs

| SKU            | Silicon                                | Status     |
|----------------|----------------------------------------|------------|
| `E1M-AEN301`   | Alif Ensemble E3                       | production |
| `E1M-AEN401`   | Alif Ensemble E4 (preliminary)         | preliminary|
| `E1M-AEN501`   | Alif Ensemble E5 (preliminary)         | preliminary|
| `E1M-AEN601`   | Alif Ensemble E6 (preliminary)         | preliminary|
| `E1M-AEN701`   | Alif Ensemble E7                       | production |
| `E1M-AEN801`   | Alif Ensemble E8 (preliminary)         | preliminary|

All AEN SKUs share the same module PCB.  Pick by Alif silicon
tier (cores + NPU count + memory).

## What's on the module

| Role                    | Part                       | Bus / signal     | Driver                                  |
|-------------------------|----------------------------|------------------|-----------------------------------------|
| Application SoC         | Alif Ensemble E3..E8       | --               | (vendor HAL)                            |
| Wi-Fi 6 + BLE 5.4       | TI CC3501E                 | inter-chip SPI1 + SDIO | App APIs: [`<alp/iot.h>`](../../include/alp/iot.h), [`<alp/ble.h>`](../../include/alp/ble.h); diagnostics: [`<alp/chips/cc3501e.h>`](../../include/alp/chips/cc3501e.h) |
| Secure element          | Infineon OPTIGA Trust M    | BRD_I2C†          | [`<alp/chips/optiga_trust_m.h>`](../../include/alp/chips/optiga_trust_m.h) |
| RTC                     | Micro Crystal RV-3028-C7   | BRD_I2C†          | Upstream Zephyr `CONFIG_RTC_RV3028` (`rtc_*` API) -- see [BRD_I2C](#on-module-housekeeping-i2c-brd_i2c) |
| Temperature sensor      | TI TMP112                  | BRD_I2C†          | Upstream Zephyr `CONFIG_TMP112` (sensor API) -- see [BRD_I2C](#on-module-housekeeping-i2c-brd_i2c) |
| EEPROM (SoM manifest)   | Onsemi N24S128             | SoC I2C2 (bridge/DNP-selected, a separate bus from BRD_I2C) | [`<alp/chips/eeprom_24c128.h>`](../../include/alp/chips/eeprom_24c128.h) |
| Ethernet PHY            | TI DP83825 (exact order code TBD) | RMII      | none -- see [`metadata/chips/dp83825.yaml`](../../metadata/chips/dp83825.yaml) |

† On the **E1M-AEN801**, BRD_I2C is SoC I2C0 (function C, `P7_0` SDA /
`P7_1` SCL), master-capable -- corrected from an earlier belief that it was
the slave-only Alif LPI2C0 (#1848) -- and **bench-proven on 2626-R2
silicon**. Full customer-facing writeup, including the limitations that
will surprise you, in [On-module housekeeping I2C (BRD_I2C)](#on-module-housekeeping-i2c-brd_i2c)
below. The other AEN SKUs still carry the pre-#1848 LPI2C0 assumption in
their own preset files pending the same netlist evidence.

Memory + per-SKU specifics: [`metadata/e1m_modules/E1M-AEN<NNN>.yaml`](../../metadata/e1m_modules/).

## On-module housekeeping I2C (BRD_I2C) {#on-module-housekeeping-i2c-brd_i2c}

**E1M-AEN801 only.** BRD_I2C is the module's own housekeeping bus: a real
time clock and a temperature sensor your application can use without a
single carrier-side component. It is **SoC I2C0, function C** -- `P7_0`
(SDA) / `P7_1` (SCL) -- driven by upstream Zephyr's `i2c_dw`
(`snps,designware-i2c`), ADR 0017 Tier-1.

| Part | 7-bit address | Fitted on this batch | Upstream driver |
|------|---------------|----------------------|-----------------|
| Micro Crystal **RV-3028-C7** RTC (U21) | `0x52` | yes | `CONFIG_RTC_RV3028`, compatible `microcrystal,rv3028` |
| TI **TMP112** temperature sensor (U20) | `0x40` | yes | `CONFIG_TMP112`, compatible `ti,tmp112` |
| Infineon **OPTIGA Trust M** (IC1) | `0x30` | **no -- DNP** | n/a |

The bus is **isolated**: the two 0 Ω jumpers that would bridge it into the
I2C2/EEPROM segment (`R93`/`R94`) are DNP, so the SoM manifest EEPROM is
**not** on BRD_I2C -- it is on SoC I2C2 (`P5_6`/`P5_7`), a separate bus.
BRD_I2C reaches no E1M edge pad either; it is module-internal.

The OPTIGA's silence at `0x30` on this batch is a population fact, not a
fault. It must not appear in a shipped devicetree.

### Using it from an application

Both parts are driven by **upstream Zephyr drivers** -- alp-sdk vendors no
code for either, and neither part needs an `<alp/*>` detour.

The bus, both device nodes and their aliases are **generated into the
E1M-AEN801 board devicetree** from
[`metadata/e1m_modules/aen/on-module-links.yaml`](../../metadata/e1m_modules/aen/on-module-links.yaml),
so an application does not have to wire any of it: `i2c0` comes up enabled
with its pinctrl group, the RV-3028 and the TMP112 appear as child nodes, and
the aliases `alp-i2c2` (the portable bus index -- `0` and `1` are the E1M
*edge* buses), `rtc` and `ambient-temp0` point at them. Each example also
carries the same wiring in its own `boards/` overlay, which is the readable
reference for what those nodes contain and why.

**If you do hand-roll the pad group, it has two traps in it**, both of which
have already cost a bench run:

* `input-enable` must be on **both** `P7_0` and `P7_1`. With it on SDA only,
  the controller NACKs every address for a reason that looks electrical.
* the bias must be `bias-pull-up` (DSC=1, a **real** pull-up). Do not copy the
  I2C2/EEPROM overlay's `bias-pull-down` (DSC=2, a pull-**down**) -- that is
  safe there only because that bus has external carrier pull-ups. This net has
  none, and a pull-down would park both lines low.

`drive-open-drain` is deliberately **not** set, leaving the pad push-pull.

Reaching the devices:

```c
/* RTC -- DT_ALIAS(rtc) works too; the examples use the node label */
const struct device *const rtc = DEVICE_DT_GET(DT_NODELABEL(rv3028));
struct rtc_time now;

rtc_get_time(rtc, &now);

/* Thermometer -- by compatible, so the app needs no node name */
const struct device *const temp =
	DEVICE_DT_GET(DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tmp112));
struct sensor_value val;

sensor_sample_fetch(temp);
sensor_channel_get(temp, SENSOR_CHAN_AMBIENT_TEMP, &val);
```

Kconfig:

```
# RTC + alarm
CONFIG_I2C=y
CONFIG_GPIO=y          # the /INT line lands on lpgpio
CONFIG_RTC=y
CONFIG_RTC_ALARM=y     # WITHOUT this the alarm half of the driver is not built
CONFIG_RTC_RV3028=y

# Thermometer
CONFIG_SENSOR=y
```

`CONFIG_RTC_RV3028` and `CONFIG_TMP112` are both `default y` off their
devicetree nodes, and `CONFIG_TMP112` additionally `select`s I2C. Naming
`CONFIG_TMP112` by hand is a mistake, not a belt-and-braces: on any target
without a `ti,tmp112` node the assignment has an unmet dependency and the
build warns. Let the devicetree drive it.

Worked examples:

* [`examples/aen/aen-rtc-alarm`](../../examples/aen/aen-rtc-alarm) -- set the
  time, arm an alarm, take the interrupt.
* [`examples/aen/aen-temp-sensor`](../../examples/aen/aen-temp-sensor) -- read
  the temperature in a loop.

[`examples/aen/aen-brd-i2c-scan`](../../examples/aen/aen-brd-i2c-scan) is the
bench probe that proved the bus. Read it for evidence, not as an application
template -- it deliberately retargets `zephyr,flash` to ITCM for a J-Link RAM
run.

### RTC alarm interrupt

The RV-3028's `/INT` output (U21 pin 2, net `RTC_ALARM`) reaches the SoC:

```
RV-3028-C7 /INT  ->  RTC_ALARM  ->  P15_0_FLEX (ball V2)  ->  LPGPIO bit 0  ->  IRQ 171
```

It is **open-drain, active low**, and `R98` (100 kΩ to `+1V8`) is fitted as
its pull-up, so no carrier-side resistor is needed. The pin is described to
the driver as `int-gpios = <&lpgpio 0 GPIO_ACTIVE_LOW>`.

> **If an alarm never fires, suspect the GPIO port before the RTC.** The
> Alif DFP notes that port 15's `LPGPIO_CTRL_n` register uses a *different
> layout* from the other GPIO ports. A misconfigured LPGPIO reads exactly
> like a dead RTC. Confirm the alarm flag over I2C first: if the RTC's
> status register shows the alarm fired, the RTC is fine and the fault is
> in the interrupt path.

### Limitations on this batch -- read these before you design around the bus

**1. The RTC does not keep time across a power cycle.** `VDD_BAT` /
`VBACKUP` (U21 pin 6) has **no supply fitted**: its only other net members
are `R4` and `R68`, both 0 Ω and both DNP. There is no backup cell and no
trickle-charge source, so the RTC loses the time *and* any pending alarm
whenever module power drops. Set the time at every boot. Persistence is a
**populate change** on the module, not a firmware setting -- ask before
you design a product around it.

Two devicetree properties follow directly from that, and are set this way
deliberately:

* `backup-switch-mode = "disabled"` -- required by the binding, and
  "disabled" is the only honest value with no cell present.
* `trickle-resistor-ohms` is **not set**. Enabling a trickle charger into
  an unpopulated `VBACKUP` net would be wrong.

**2. `RTC_CLKOUT` is carrier-only -- firmware cannot use it.** The RV-3028's
`CLKOUT` (U21 pin 1) goes **only** to the E1M edge connector, pin **AH16**.
It does not reach the SoC. It is therefore a *carrier designer's* clock
source: pick a frequency with the `clkout-frequency` property if your
carrier consumes it. Left omitted -- the shipped default -- the pin stays
low.

**3. `MODULE_STBY` / `EVI` is a carrier input.** The RV-3028's external
event input (U21 pin 8) comes from E1M edge pin **O2**, with `R43` (100 kΩ
to `+1V8`). Nothing on the module drives it; timestamping an event there is
a carrier design decision.

### Bench evidence

Measured 2026-09-05 on an E1M-AEN801 **2626-R2** module (Flow A, cold-cycle
proven), `i2c0` at 100 kHz:

* **The bus works on the SoC's internal pull-up alone.** No external
  pull-up resistor is fitted anywhere on the net, and none is needed:
  `R93`/`R94` stay DNP.
* **RV-3028-C7 @ `0x52`:** ACK. ID register `0x28` reads `0x44` -- per the
  RV-3028-C7 Application Manual Rev. 1.4 §3.14 the high nibble (HID `0x4`)
  is the hardware-identity field and matches; the low nibble (VID `0x4`) is
  a production-line code, not an identity claim. The seconds register
  advanced `0x01` → `0x02`, so the oscillator runs.
* **TMP112 @ `0x40`:** ACK. `0x40` is the design address for U20's exact
  orderable MPN (`TMP112DIDPWR`, X2SON-5 package per SBOS473L p.44 + Table
  7-4, ADD0→GND). Reads back a plausible temperature (28.062 °C on
  2026-09-05; 27.687 °C on 2026-09-07, serial `2026W36-0002`), and
  fingerprints TMP112-shaped (`CONFIG` `0x60a0`, `T_LOW` `0x4b00`, `T_HIGH`
  `0x5000` -- the datasheet power-on defaults).
* **Every non-response was a clean `rc=-5` (`-EIO`) NACK** -- zero
  `-ETIMEDOUT`, zero `User Abort on i2c@49010000` in the whole run. That
  distinction matters when you debug this bus: a NACK means the controller
  is driving a healthy wire and nobody answered that address, whereas a
  timeout plus `User Abort` means the pads never reached the wire at all
  (usually a pinctrl error, not a missing device).

An earlier probe, dated 2026-08-31, concluded that BRD_I2C was "not usable
as built" and needed `R93`/`R94` stuffed. **That conclusion does not
carry.** It was run on an **r1** module, where the RTC and TMP112 sit on
LPI2C0 (`P7_4`/`P7_5`) and are not connected to `P7_0`/`P7_1` at all --
nothing could have ACKed on those pads whatever the biasing, so the run
measured two floating pins rather than a populated bus. The measurement was
real; the inference drawn from it was about the wrong revision.

## Carrier requirement: an external brownout supervisor on POR_N

**The module carries no reset or brownout supervisor, and omitting one on the
carrier can permanently damage the SoC.**

Errata `AERR0012` v2.0 `ER004` applies to every Ensemble E4/E6/E8 revision with
no fix planned:

> During this 10ms period, the power supply voltage must rise monotonically, and
> the voltage on the VDD_MAIN and VDD_BUCK pins must never drop below 1.65V once
> 1.65V is reached. If this cannot be guaranteed, an external reset supervisor
> device must be used to drive (active low) the Ensemble system reset pin,
> POR_N, to an active state while the power supply voltage is below 1.65V.

> If the voltage profile outlined in the description above is not met, and an
> external reset supervisor device is not used, there is potential to damage the
> Ensemble SoC rendering it non-functional.

Datasheet `ADTS0013` v1.2 §5.2.1 has since promoted this from an erratum to a
normative operating condition:

> An external brownout supervisor must be connected to VDD_MAIN/VDD_BUCK. The
> brownout supervisor must assert before voltage input to the chip falls below
> 1.65V. Care (that is, sufficient decoupling) must be taken to ensure that
> supply noise/transients do not spuriously trigger resets.

On the `E1M-AEN-2626-R2` module, `POR_N` is pulled up on-module and brought out
to the E1M edge connector; nothing on the module drives it. So the supervisor,
and the decoupling that keeps it from tripping spuriously, belong to the
carrier design. The E1M-EVK is a development carrier -- do not infer from it
that a product carrier can skip this.

## CC3501E coprocessor

The AEN module's Wi-Fi 6 + BLE 5.4 ride a dedicated TI CC3501E
co-MCU rather than driver-level Wi-Fi on the Alif silicon.  Application
code stays on the portable APIs:

```c
alp_wifi_t *wifi = alp_wifi_open();
alp_ble_t *ble = alp_ble_open();
```

The AEN bridge helper initialises the CC3501E once and attaches its live handle
behind those dispatchers. Use
[`<alp/chips/cc3501e.h>`](../../include/alp/chips/cc3501e.h) and
[`alp companion`](../cc3501e-companion-commands.md) for diagnostics, raw scan
records, sockets, OTA, and bridge health.

Boot model + architecture: [`docs/cc3501e-bridge.md`](../cc3501e-bridge.md).

## Boot + identification

The SoM's identity comes solely from the on-module EEPROM manifest
(family / SKU / hw_rev / serial / mfg date) — there is no SoM-side
ADC cross-check.  A carrier-board BOARD_ID resistor divider, where
present, identifies the *carrier* revision and is independent of the
SoM revision; it is not yet wired into `alp_hw_info_read()`.  See
[`docs/board-id.md`](../board-id.md).

## Rotary encoder (QEC0 / UTIMER channel 12) -- bench state {#rotary-encoder-qec0-bench-state}

The E1M EVK's `PEC12R-4222F-S0024` (24 PPR) reaches the SoC as `ENC0_X` → E2
`A10` → `P3_0` and `ENC0_Y` → E2 `B10` → `P3_1`
(`metadata/e1m_modules/aen/from-alif.tsv`), decoded by **UTIMER channel 12** --
QEC0 is channel 12, not channel 0.  Binding the qdec under `utimer0` reads a
counter that never sees the encoder edges.

**The decode path is not yet proven.**  Two defects were found on
`E1M-AEN803` serial `2026W36-0002`; the first is fixed, the second is open.

* **Withdrawn (#2037), and worth reading before you touch this driver.** The
  driver leaves `CNTR_CTRL` (`0x4800D080`) = `0x00000021` — `CNTR_EN` and
  `CNTR_TRIG` set, bit 1 `RUNNING` clear — and `GLB_CNTR_RUNNING`
  (`0x4800000C`) = `0x00000000`. That was read as "the channel was never
  started", and a `GLB_CNTR_START` write was added. **It is the correct resting
  state for a trigger-counting channel, and starting the channel broke it.**

  With the start call in place and the encoder untouched, `CNTR`
  (`0x4800D0A0`) advanced 3,999,905,225 counts in 10.000 s = **400,010,738
  counts/s** — with `UP_1_SRC` and `DOWN_1_SRC` both written `0x00000000`, so
  no quadrature transition could contribute. Writing `GLB_CNTR_STOP`
  (`0x48000004`) bit 12 froze it instantly: three `CNTR` reads 5 s apart,
  bit-identical `0xF7E3EFEE`. At the shipped reload (`CNTR_PTR` =
  `0x0000005F`) that free-run wraps a revolution every 240 ns, so the reported
  angle was uncorrelated noise — worse than the stuck-at-zero symptom it was
  meant to fix.

  Alif's own QEC flow never starts the channel: `qec0_app()` in
  `demo_qec.c` runs `ConfigCounter(TRIGGERING, TRIANGLE)` → `SetCount` → three
  `ConfigTrigger` calls → `GetCount` → `Stop`, with no `Start()` anywhere.

  **Register trap:** `CNTR_CTRL` bit 1 `RUNNING` is status, not control. It is
  set by `GLB_CNTR_START` and cleared by `GLB_CNTR_STOP`; writing `0x00000023`
  into `CNTR_CTRL` reads back `0x00000021`. Watching that bit change is not
  proof that your write did anything.

* **Resolved (#2038): the counter advanced on a stationary encoder.** Cause:
  the withdrawn `GLB_CNTR_START` write above, nothing else. Five raw `CNTR`
  reads over ~27 s while it was in place showed `0x0000002D`, `0x0000001B`,
  `0x0000005F`, `0x00000039`, `0x0000003E`; removing the start call removes the
  free-run. Before the cause was found, five candidates were eliminated on the
  bench, each at the cost of a reservation, and they stay eliminated: the pads
  (four configurations including `0x00290000` = AF 0, `P3_0`/`P3_1` deselected
  from the QEC entirely — still counting); the asymmetric input filter
  (`FILTER_CTRL_B` was `0x00000000` against `FILTER_CTRL_A` `0x00100101`;
  fixed, still counting); the channel driving its own input (`GLB_DRIVER_OEN`
  covers channels 0-11 only); Sawtooth-vs-Triangle (`CNTR_CTRL` written
  `0x00000033`, still counting); and the trigger path itself (`CNTR_TRIG`
  cleared, still counting). Every one of those was measured against a channel
  that was free-running for a reason none of them addressed — which is why they
  all came back negative.

* **Still open: the original stuck-at-zero reading is unexplained.** That
  symptom is what started this, and "the counter was never started" was the
  wrong explanation for it. With the channel in its correct resting state,
  whether it increments on real quadrature edges has never been observed,
  because no run has had a hand on the shaft. `UP_0_SRC` (`0x18`) and
  `DOWN_0_SRC` (`0x20`) read `0x00000000`; whether that is a defect is open.

**Two traps when you measure this.**  The counter wraps at its programmed
reload (`CNTR_PTR` = `counts-per-revolution - 1` = `0x0000005F`), so neither
the app's printed degrees nor a raw `CNTR` read can distinguish "static" from
"advanced by exactly 96·k" -- no counts-per-second figure is derivable from
either.  Widen `CNTR_PTR` to `0xFFFFFFFF` first if you need a rate; that is how
the 400 Mcount/s free-run was finally measured.  And **watching `CNTR_CTRL` bit
1 `RUNNING` change is not proof your write landed** — it is status, set by
`GLB_CNTR_START` and cleared by `GLB_CNTR_STOP`, and a write of `0x00000023`
into `CNTR_CTRL` reads back `0x00000021`.

**An attended run is now the right next step**, which it was not while #2038
stood: with the free-run gone, a count that moves when someone turns the shaft
means the decode works, and one that does not move is the original defect
reproduced under a hand.  Turn one detent (expect ±4 raw counts at ×4 decode),
then one full revolution each way.

`counts-per-revolution` is **96** -- 24 PPR × 4 for the driver's x4 decode --
and the qdec driver writes it into the hardware counter's reload register, so
it sets the real wraparound rather than a display scale.

## Bring-up

The general bring-up procedure in
[`docs/getting-started.md`](../getting-started.md) covers AEN as
the first-class target.  AEN ships at v0.1; most cross-family
examples already work against it.

## Pins

* `metadata/e1m_modules/aen/from-alif.tsv` -- E1M pad → Alif silicon.
* `metadata/e1m_modules/aen/from-cc3501e.tsv` -- E1M pad → CC3501E MCU.
* `metadata/e1m_modules/aen/inter-chip.tsv` -- Alif ↔ CC3501E wiring.

## Example apps targeting AEN

All cross-family examples + the AEN-specific ones:

| Example                          | What you'll see                                             |
|----------------------------------|-------------------------------------------------------------|
| `edgeai-vision-aen`              | On-device inference with Ethos-U on AEN silicon.            |
| `iot-connected-camera`           | Capture + MQTT publish via the CC3501E.                     |

Plus every cross-family example.

## See also

* [`v2n.md`](v2n.md) -- larger form factor + Renesas-based.
* [`../cc3501e-bridge.md`](../cc3501e-bridge.md) -- inter-chip protocol.
* [`../firmware-quickstart.md`](../firmware-quickstart.md) -- cross-family FW patterns.
