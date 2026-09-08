# aen-evk-demo

Phased full-board demo for the **E1M-AEN801** (Alif Ensemble E8, M55-HE) on
the **E1M EVK** carrier. Bench target: `e1m-aen-evk-03` (E1M-AEN803 serial
2026W36-0002, EVK rev 2626-R2).

## Why a phase framework

`i2c-device-hub` used to print `RESULT PASS: 13/13` while one of its sensors
was still returning its power-on-reset sentinel -- the tally counted a
successful ID *read* as a pass, not a successful *sample*. This app is
shaped to make that class of bug structurally impossible: every phase
returns exactly one of three verdicts, and the summary always reports all
three counts together so a run full of `SKIPPED` phases (no SD card, nobody
at the bench) never reads as a failed run.

| Verdict | Meaning |
|---|---|
| `PASS` | The phase exercised its hardware and the data was valid. |
| `SKIPPED` | The hardware it needs is absent, or no operator is present to do the physical part. **Never `PASS`.** |
| `FAIL` | The hardware is present and it misbehaved. |

## Scope of this slice

Thirteen phases run in a fixed order. The first six are fully implemented
against bench-proven drivers; the remaining seven are stubs that always
report `SKIPPED`, each with its own reason (see `src/main.c`'s "STUBS"
section) -- an attended-run requirement, a larger deferred unit of work,
and "no panel on this bench" are different kinds of gaps and are described
as such, not collapsed into one generic "not implemented".

| # | Phase | Bus / hardware | Asserts | Cannot assert |
|---|---|---|---|---|
| 1 | RTC + temperature | BRD_I2C (SoC I2C0) | RV-3028-C7 `init`/`was_cold_start`/`get_time`; seconds advance across a 1 s gap. TMP112 reading inside a plausible indoor band. | RTC accuracy/drift; TMP112 absolute accuracy (plausibility only). |
| 2 | Sensors | carrier bus (SoC I2C2) | BMI323 / ICM-42670 / BMP581: documented startup floor, then a **bounded poll** of the chip's own data-ready flag, then a read; rejects the documented invalid/reset sentinels (`0x8000` per axis, `0x7f7f7f` raw). | Sensor calibration/accuracy; motion content of the sample. |
| 3 | Power rails | carrier bus | All six INA236: bus voltage, **shunt microvolts**, current, gated on `ina236_conversion_ready()` having been observed set. | Whether a 0 mV/0 uV reading is "correct" for a given rail -- `+VCAM0`/`+VCAM1` are expected 0 (no camera fitted) and `+1V8`'s 0 uV shunt is an open question this demo reports but does not resolve. |
| 4 | I/O expander | carrier bus | TCAL9538 configuration (0x03) and input port (0x00) register reads; also reads interrupt status (0x46) read-only (nothing here unmasks 0x45, so nothing is pending to acknowledge). | Any interrupt actually routing through a sensor -- see `aen-sensor-int-probe` for that. |
| 5 | EEPROM identity | carrier bus | 24C128 read-only: `"ALPH"` magic + `"aen"` family string at the start of the manifest. | Manifest field accuracy beyond magic/family (see `aen-eeprom-manifest` for the full CRC/field decode). |
| 6 | RGB LED | PWM0 (red) / PWM3 (green) / PWM1 (blue) | Drives each channel via `<alp/pwm.h>`, then asserts the UTIMER **register** the driver programmed (driver-enable + compare-enable bits, clock gate, run bit, a real fractional duty) -- restores each channel to idle before returning. | That the LED visibly lit -- unverifiable with no operator present; claiming it would be the same class of lie as counting an ID read as a pass. |
| 7 | Rotary encoder | -- | Nothing (stub). | Needs an attended run (someone turning the knob) -- see `EVK-BRIEFING.md`'s open question about whether the driver can even distinguish "no motion" from "not counting". |
| 8 | CC3501E Wi-Fi/BLE | -- | Nothing (stub). | Bridge protocol dispatch is a larger unit of work deferred to the next slice, not a hardware gap -- the coprocessor is already activated on every SoM. |
| 9 | SD card | -- | Nothing (stub). | Blocked on phase 8: the SDIO mux sits behind the CC3501E bridge. |
| 10 | Ethernet | -- | Nothing (stub). | Deferred to the next slice. |
| 11 | Sound out -> PDM in | -- | Nothing (stub). | I2S bring-up + the low-volume ramp policy for the ~15 W class-D amps deferred to the next slice. |
| 12 | Screen (DSI) | -- | Nothing (stub). | No panel on this bench; a clean DSI init would not prove one is attached anyway. |
| 13 | JPEG + NPU | -- | Nothing (stub). | Heaviest phase in the full design, deferred to keep this slice bench-provable. Not camera-gated -- JPEG encodes its own gradient, the NPU runs its own model. |

## Buses

- **BRD_I2C** -- SoC I2C0, portable bus index 2 (alias `alp-i2c2`): RV-3028-C7
  `0x52`, TMP112 `0x40`. On-module housekeeping bus; SoM-specific.
- **carrier bus** -- SoC I2C2, portable bus index 0 / `ALP_E1M_I2C0` (alias
  `alp-i2c0`, pads P5_6 SCL / P5_7 SDA): BMI323 `0x68`, ICM-42670 `0x69`,
  BMP581 `0x47`, six INA236, TCAL9538 `0x73`, and the SoM's own 24C128
  manifest EEPROM `0x50` (bridge/DNP-selected onto the same physical bus).

Both are already enabled by the board layer
(`metadata/e1m_modules/aen/on-module-links.yaml`, `metadata/pinmux/aen.yaml`)
-- this app's own overlay adds nothing for either. Confusing the two buses
wastes a bench run; see `EVK-BRIEFING.md`.

## RGB LED PWM routing

The netlist net labels for this LED are **wrong**; the mapping below is
bench-measured and already folded into
`<alp/boards/alp_e1m_evk_routes.h>`'s `EVK_PWM_LED_*` macros, which this app
consumes rather than re-deriving:

| Colour | Channel | UTIMER | Pad |
|---|---|---|---|
| Red | `ALP_E1M_PWM0` | utimer11, driver B | P12_7 |
| Blue | `ALP_E1M_PWM1` | utimer11, driver A | P12_6 |
| Green | `ALP_E1M_PWM3` | utimer10, driver A | P2_4 |

Red and blue share one physical UTIMER block on its two independent driver
outputs; green is a separate block. The PWM driver
(`zephyr/drivers/pwm/pwm_alif_utimer.c`) is marked INTERIM /
BENCH-UNVERIFIED in its own file header -- this is the first app to load it
on real silicon, which is why this phase verifies by register readback
(the same approach `aen-pwm-utimer-pwmleds` uses) instead of assuming.

## Hard constraints this app respects

- Never calls `rv3028c7_set_int_enable()` or `rv3028c7_route_clkout()` --
  both write EEPROM whose documented endurance is 100 cycles minimum at the
  hot corner.
- The EEPROM phase is read-only: `eeprom_24c128_write()` is never called.
- The I/O-expander phase never unmasks an interrupt source (0x45) -- it
  only demonstrates that the Agile-IO block answers, read-only.
- No CC3501E activation or firmware-flash path exists in this app (the
  phase is a stub).

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```bash
source /home/caner/alp-env.sh   # ZEPHYR_BASE, toolchain
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-evk-demo -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-this-alp-sdk-checkout>;<path-to-hal_alif>"
```

Confirmed building clean against this tree:

```
Memory region         Used Size  Region Size  %age Used
           FLASH:       94700 B       256 KB     36.13%
             RAM:       12136 B       256 KB      4.63%
```

## Console

This bench's app UART emits nothing under the Flow C RAM-run this app
targets and exports no SE-UART. `prj.conf` carries the RAM-console toggle
every sibling AEN bench app documents -- comment the four `UART_CONSOLE`
lines and uncomment the `RAM_CONSOLE` pair to read `ram_console_buf` over
SWD instead.

## Expected output shape

```
[evkdemo] phase  1/13: RTC + temperature (BRD_I2C)         PASS
[evkdemo] phase  2/13: Sensors (BMI323/ICM42670/BMP581)     PASS
[evkdemo] phase  3/13: Power rails (6x INA236)              PASS
[evkdemo] phase  4/13: I/O expander (TCAL9538)              PASS
[evkdemo] phase  5/13: EEPROM identity (24C128)             PASS
[evkdemo] phase  6/13: RGB LED (PWM0/1/3)                   PASS
[evkdemo] phase  7/13: Rotary encoder                       SKIPPED
[evkdemo] phase  8/13: CC3501E Wi-Fi/BLE                    SKIPPED
[evkdemo] phase  9/13: SD card                              SKIPPED
[evkdemo] phase 10/13: Ethernet                             SKIPPED
[evkdemo] phase 11/13: Sound out -> PDM in                  SKIPPED
[evkdemo] phase 12/13: Screen (DSI)                         SKIPPED
[evkdemo] phase 13/13: JPEG + NPU                           SKIPPED
...
[evkdemo] RESULT: 6 PASS, 7 SKIPPED, 0 FAIL
[evkdemo] done
```

A run with skips is not a failed run -- the three counts are always
reported together.

## Reference

- `EVK-BRIEFING.md` / `DEMO-DESIGN.md` (dispatch scratchpad) -- the verified
  board facts and phase design this app implements.
- [`examples/peripheral-io/i2c-device-hub`](../../peripheral-io/i2c-device-hub/)
  -- the data-ready-poll pattern phases 1-2 copy, and the PASS/PARTIAL bug
  this app's whole verdict design exists to avoid repeating.
- [`examples/aen/aen-sensor-int-probe`](../aen-sensor-int-probe/) --
  routed-interrupt consumer pattern for the TCAL9538 (not exercised by
  phase 4 here, which is read-only).
- [`examples/aen/aen-pwm-utimer-pwmleds`](../aen-pwm-utimer-pwmleds/) --
  the register-readback verification pattern phase 6 reuses.
- [`<alp/boards/alp_e1m_evk_routes.h>`](../../../include/alp/boards/alp_e1m_evk_routes.h)
  -- `EVK_I2C_ADDR_*` / `EVK_PWM_LED_*` map.
