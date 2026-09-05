# aen-temp-sensor

Read the E1M-AEN801's **on-module TMP112** temperature sensor through the
**upstream Zephyr sensor API** — `CONFIG_SENSOR` + `CONFIG_TMP112`,
`SENSOR_CHAN_AMBIENT_TEMP` — and print the value as integer milli-degrees C.

The starter app for "how do I read a sensor on this SoM". It is the AEN
counterpart of [`v2n-temp-sensor`](../../v2n/v2n-temp-sensor), which reads the
same part on the V2N through the alp-sdk chip driver instead.

## Layering

| Layer | What it owns |
| --- | --- |
| Board (`zephyr/boards/alp/e1m_aen801_m55_he/`) | BRD_I2C — SoC I2C0, `i2c@49010000`, `"snps,designware-i2c"`, pads P7_0 `I2C0_SDA_C` / P7_1 `I2C0_SCL_C` — **and** the `"ti,tmp112"` child node on it at `reg = <0x48>`. |
| Driver | Upstream `drivers/sensor/ti/tmp112/`, binding `dts/bindings/sensor/ti,tmp112.yaml`. |
| App (`src/main.c`) | `sensor_sample_fetch_chan()` + `sensor_channel_get()`. No I2C register pokes, no chip driver, no vendor header. |

That is ADR 0017 Tier-1 (upstream-native): alp-sdk contributes the board
devicetree and nothing else, because upstream already drives this part. The
example's own overlay
(`boards/alp_e1m_aen801_m55_he_ae822fa0e5597ls0_rtss_he.overlay`) is therefore
**empty on purpose** — the TMP112 is a SoM fact, so every AEN801 app inherits
it from the board layer rather than each one re-declaring it. The file still
has to exist under its fully-qualified name, because that is the only name
Zephyr auto-applies.

`CONFIG_TMP112` is not written into `prj.conf`: upstream declares it
`default y depends on DT_HAS_TI_TMP112_ENABLED`, so the enabled DT node turns
the driver on and `select I2C` pulls the bus in behind it.

## Build

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-temp-sensor
```

or, on the bench:

```bash
scripts/bench/aen/build.sh "$PWD/examples/aen/aen-temp-sensor"
```

This is a flashed Flow A / slot0 target (`CONFIG_FLASH_LOAD_OFFSET=0x10000`),
not an ITCM RAM-run. For Flow C, add
`scripts/bench/aen/aen-flowc-itcm.{conf,overlay}` on the build command line.

## What it shows

1. Bind the sensor by **compatible** (`DT_COMPAT_GET_ANY_STATUS_OKAY(ti_tmp112)`),
   not by node label — the compatible is the stable contract, the label is the
   board layer's business.
2. Poll: 8 samples 500 ms apart, each one printed. The TMP112's default
   `conversion-rate` is 4 Hz, so every fetch returns a fresh conversion.
3. Print **integer milli-degrees C**. `sensor_value` is already an exact
   integer pair, and `sensor_value_to_milli()` collapses it without an FPU —
   no `%f`, which on a minimal-libc bench config prints nothing at all and
   reads on the console as a dead sensor.
4. Sanity-band each reading against 15000..35000 milli-degC. **This is a
   plausibility check, not a correctness claim**: inside the band is not proof
   of accuracy and outside it is not proof of a fault. It exists to separate
   "this looks like a temperature" from "this looks like a mis-decoded
   register", and a cold-chamber or outdoor run legitimately falls outside it.
5. One machine-greppable verdict:

   | Line | Meaning |
   | --- | --- |
   | `RESULT PASS` | Every sample read, every one inside the plausibility band. |
   | `RESULT PARTIAL` | The sensor answers, but some fetches dropped or some values fell outside the band. |
   | `RESULT FAIL` | No `ti,tmp112` node bound, the device failed to initialise, or every fetch failed. |

## The 0x48 / 0x40 address anomaly

**The devicetree address is `0x48` and that is correct.** The
E1M-AEN-2626-R2 netlist ties `U20` pin 3 (`ADD0`) to 0V, and the TMP112 strap
table (TI SBOS397) maps `ADD0 → GND = 0x48`, `→ V+ = 0x49`, `→ SDA = 0x4A`,
`→ SCL = 0x4B`.

**One bench module (2026-09-05) answered at `0x40` instead.** The part was
genuine and healthy — confirmed a TMP112 by a three-of-three register
fingerprint against the datasheet power-on defaults (`CONFIG=0x60a0`,
`T_LOW=0x4b00`, `T_HIGH=0x5000`) and reading back 28.062 °C. Only its address
was wrong. `0x40` is **not a legal TMP112 address**, so on that unit `ADD0` is
not actually sitting at GND: the suspected cause is an **open joint on U20 pin
3**. That is a per-unit board defect, not a design error and not a firmware
bug.

So when the sensor does not respond, this example prints an actionable
diagnostic naming `0x40` as a known observed anomaly, and tells you to check
continuity from U20 pin 3 to GND. It deliberately does **not** silently probe
`0x40` and carry on: firmware that papers over a bad joint ships that defect to
every customer. If you want to see what is really on the bus, run
[`aen-brd-i2c-scan`](../aen-brd-i2c-scan), which scans every 7-bit address and
fingerprints whatever answers.

Do not change the devicetree to `0x40`.

## Expected output

```
=== aen-temp-sensor: on-module TMP112 via the Zephyr sensor API ===
TMP112 "tmp112@48" ready at devicetree address 0x48 (design address, U20 ADD0 tied to 0V)
taking 8 samples 500 ms apart ...
  sample 1/8: 28062 milli-degC
  ...
RESULT PASS: 8/8 TMP112 samples read at 0x48, all within the plausible indoor band (plausibility check, not an accuracy claim)
```

> **NOT YET BENCH-RUN.** The temperature read itself has been proven on
> 2626-R2 silicon by `aen-brd-i2c-scan` (raw register read, 28.062 °C); this
> app's upstream-driver path is build-verified only until it gets a bench slot.
