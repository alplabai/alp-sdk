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

## On-module housekeeping I2C (BRD_I2C)

**E1M-AEN801 only.** BRD_I2C is the module's own housekeeping bus: a real
time clock and a temperature sensor your application can use without a
single carrier-side component. It is **SoC I2C0, function C** -- `P7_0`
(SDA) / `P7_1` (SCL) -- driven by upstream Zephyr's `i2c_dw`
(`snps,designware-i2c`), ADR 0017 Tier-1.

| Part | 7-bit address | Fitted on this batch | Upstream driver |
|------|---------------|----------------------|-----------------|
| Micro Crystal **RV-3028-C7** RTC (U21) | `0x52` | yes | `CONFIG_RTC_RV3028`, compatible `microcrystal,rv3028` |
| TI **TMP112** temperature sensor (U20) | `0x48` (ADD0 strapped to GND) | yes | `CONFIG_TMP112`, compatible `ti,tmp112` |
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
  the temperature in a loop, and report the `0x40` anomaly below if it sees it.

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

**4. One bench module answered at `0x40` instead of `0x48`.** `0x40` is not
a legal TMP112 address at all -- the strap table is ADD0→GND `0x48`,
→V+ `0x49`, →SDA `0x4A`, →SCL `0x4B` -- and that unit's part was confirmed a
genuine TMP112 by a three-of-three register fingerprint against the
datasheet power-on defaults (`CONFIG` `0x60a0`, `T_LOW` `0x4b00`, `T_HIGH`
`0x5000`), reading 28.062 °C. So it is a **defect on that one module**, not
the design: ADD0 is not actually sitting at GND. The design address is
`0x48` and the shipped devicetree uses `0x48`.

> **How to spot it:** nothing ACKs at `0x48`, but a device ACKs at `0x40`.
> Check **U20 pin 3 (ADD0) for continuity to GND** on that module. Do not
> re-point the devicetree at `0x40` -- that hides a board fault and breaks
> every good module.

### Bench evidence

Measured 2026-09-05 on an E1M-AEN801 **2626-R2** module (Flow A, cold-cycle
proven), `i2c0` at 100 kHz:

* **The bus works on the SoC's internal pull-up alone.** No external
  pull-up resistor is fitted anywhere on the net, and none is needed:
  `R93`/`R94` stay DNP.
* **RV-3028-C7 @ `0x52`:** ACK. ID register `0x28` reads `0x44`; the
  seconds register advanced `0x01` → `0x02`, so the oscillator runs.
* **TMP112 @ `0x48`** (design address; see limitation 4): ACK, fingerprint
  matches, 28.062 °C.
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
