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

Fourteen phases run in a fixed order. Eight are fully implemented against
bench-proven drivers -- the first six, plus phase 8 (the CC3501E Wi-Fi 6 /
BLE 5.4 coprocessor over the inter-chip SPI bridge) and phase 13 (JPEG
encode on the Hantro VC9000E); the remaining six are stubs that always
report `SKIPPED`, each with its own reason (see `src/main.c`'s "STUBS" section)
-- an attended-run requirement, a larger deferred unit of work, "no panel
on this bench", and a model that would need a different **boot flow** are
different kinds of gaps and are described as such, not collapsed into one
generic "not implemented".

Neither phase 13 nor phase 14 is camera-gated: the JPEG phase encodes a
synthetic gradient it builds itself and the NPU model carries its own
input. No camera module is required by, or in scope for, either.

| # | Phase | Bus / hardware | Asserts | Cannot assert |
|---|---|---|---|---|
| 1 | RTC + temperature | BRD_I2C (SoC I2C0) | RV-3028-C7 `init`/`was_cold_start`/`get_time`; seconds advance across a 1 s gap. TMP112 reading inside a plausible indoor band. | RTC accuracy/drift; TMP112 absolute accuracy (plausibility only). |
| 2 | Sensors | carrier bus (SoC I2C2) | BMI323 / ICM-42670 / BMP581: documented startup floor, then a **bounded poll** of the chip's own data-ready flag, then a read; rejects the documented invalid/reset sentinels (`0x8000` per axis, `0x7f7f7f` raw). | Sensor calibration/accuracy; motion content of the sample. |
| 3 | Power rails | carrier bus | All six INA236: bus voltage, **shunt microvolts**, current, gated on `ina236_conversion_ready()` having been observed set. | Whether a 0 mV/0 uV reading is "correct" for a given rail -- `+VCAM0`/`+VCAM1` are expected 0 (no camera fitted) and `+1V8`'s 0 uV shunt is an open question this demo reports but does not resolve. |
| 4 | I/O expander answers | carrier bus | TCAL9538 configuration (0x03) and input port (0x00) register reads; also reads interrupt status (0x46) read-only (nothing here unmasks 0x45, so nothing is pending to acknowledge). | Any interrupt actually routing through a sensor -- see `aen-sensor-int-probe` for that. |
| 5 | EEPROM identity | carrier bus | 24C128 read-only: `"ALPH"` magic + `"aen"` family string at the start of the manifest. | Manifest field accuracy beyond magic/family (see `aen-eeprom-manifest` for the full CRC/field decode). |
| 6 | RGB LED | PWM0 (red) / PWM3 (green) / PWM1 (blue) | Drives each channel via `<alp/pwm.h>`, then asserts the UTIMER **register** the driver programmed (driver-enable + compare-enable bits, clock gate, run bit, a real fractional duty), then holds the colour lit for 1500 ms and names the colour it expects -- restores each channel to idle before returning. | That the LED visibly lit, and which colour it lit. The hold makes that checkable **by an operator watching the board**; the verdict itself is still the register read-back, and every channel programs identically, so a wrong `EVK_PWM_LED_*` colour mapping would still PASS. Only the eye settles the mapping. |
| 7 | Rotary encoder | -- | Nothing (stub). | Needs an attended run (someone turning the knob) -- see `EVK-BRIEFING.md`'s open question about whether the driver can even distinguish "no motion" from "not counting". |
| 8 | CC3501E Wi-Fi/BLE | inter-chip SPI1 (P14_6/5/4 + hardware SS0 P14_7), WIFI_EN P15_5, nRESET P15_1_FLEX, READY P2_6 | Powers (`WIFI_EN` high) and resets the coprocessor -- **nothing answers before this**, its supply is host-gated -- then `PING` (`0x00`) with a bounded 25 x 200 ms retry, `GET_VERSION` (`0x01`) **compared against `ALP_CC3501E_PROTOCOL_VERSION`**, `GET_MAC` (`0x03`) checked for a structurally valid station address, `GET_CAPABILITIES` (`0x06`), a passive `WIFI_SCAN_START` (`0x10`), and `BLE_ENABLE` (`0x30`). Every return code is printed. `PASS` requires **all five** of version-match, valid MAC, capabilities readable, scan round-tripped, BLE up -- a `PING` alone is explicitly not enough. | Signal quality, throughput, or that any network is reachable -- it never associates. An **empty scan is `PASS`-but-`UNCORROBORATED`**: zero networks is a statement about the RF environment, not about this board, so the gate is that the scan *round-tripped*, not that it found anything. Which colour of failure a dead link is (power / pinmux / firmware) -- the log names the three to check. |
| 9 | SD card | -- | Nothing (stub). | No longer blocked on phase 8 (which now powers and resets the coprocessor and leaves it bound) -- what remains is the SD side itself: the SDIO mux sequence on CC35 `GPIO_26`/`GPIO_30`, which additionally needs `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY` and a route table this app does not yet carry. |
| 10 | Ethernet | -- | Nothing (stub). | Deferred to the next slice. |
| 11 | Sound out -> PDM in | -- | Nothing (stub). | I2S bring-up + the low-volume ramp policy for the ~15 W class-D amps deferred to the next slice. |
| 12 | Screen (DSI) | -- | Nothing (stub). | No panel on this bench; a clean DSI init would not prove one is attached anyway. |
| 13 | JPEG encode | Hantro VC9000E @ `0x49044000` (`jpeg0`) | Encodes a synthetic 64x64 NV12 gradient through `<alp/jpeg.h>`. Prints which backend won (`caps.hw_accelerated`) and **fails a software-fallback win** -- on this board the hardware encoder is the phase. Asserts the output really is a JPEG: SOI `FF D8 FF` at the start, EOI `FF D9` at the end, and a plausible length (>= 256 B, < the 6144 B source). Every return code is printed verbatim. | The image is *correct* -- the checks are structural, not a decode. The Hantro hardware-ID readback: `<alp/jpeg.h>` exposes no accessor for `JPEG_SWREG0`, and this example will not hand-roll a register poke. A mismatch against `JPEG_HW_ID` (`0x90001000`) still surfaces, as `alp_jpeg_open() == NULL` with `ALP_ERR_NOT_READY` plus the driver's own `"JPEG hardware not found (ID: 0x%08x)"` `LOG_ERR` line (this app builds `CONFIG_LOG=y`). |
| 14 | NPU inference | -- | Nothing (stub). | **A boot-flow change, not a phase.** `aen-npu-inference-alp` is the silicon-proven Ethos-U85 path through `<alp/inference.h>`, but its Vela-compiled `person_detect_u85` model is **~263 KiB** -- which is exactly why that app links into MRAM slot0 and boots via Flow D. This demo is a **Flow C ITCM RAM-run**: ITCM is **256 KB total** and the demo already uses **about 140 KB (54.66%)** of it -- roughly 93 KB (36.24%) before phase 13, about 106 KB (41.41%) after it, and phase 8's CC3501E bridge driver added ~34 KB more, so the headroom is shrinking, not growing. The model does not fit alongside it, so adding NPU here means relinking the whole demo into MRAM slot0. Shrinking the model to fit would swap a proven artefact for an unproven one. |

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

A third bus is **not** shared through `demo_ctx_t`:

- **SPI1** -- the SoM-internal Alif <-> CC3501E inter-chip link (SCK P14_6,
  MOSI P14_5, MISO P14_4, hardware SS0 P14_7, plus `WIFI_EN` P15_5, nRESET
  P15_1_FLEX and `READY` P2_6). Phase 8 owns it end to end: it has to power
  the coprocessor before there is anything on the far end of the bus, and no
  other phase touches it. Because these are SoM-internal nets rather than E1M
  edge pads, the board layer does **not** publish them -- this app's overlay
  declares them, transcribed from `aen-cc3501e-bringup`'s.

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
- **No CC3501E activation, provisioning or firmware-flash path exists in
  this app, and none may be added.** The parts ship already activated from
  SoM provisioning, no SDK opcode can activate one, and the fuses involved
  are OR-only -- a botched activation permanently bricks a unit's secure
  boot. The driver's OTA opcodes (`cc3501e_ota_*`) are deliberately never
  called here. Radio operations -- scan, BLE enable -- are ordinary runtime
  commands and are what phase 8 is for.
- Phase 8 **never joins a network**: it scans and stops. `cc3501e_wifi_connect()`
  is not called and there are no credentials in this app or its build.

## Build

Standalone Zephyr app (no `alp_project.py` board.yaml flow):

```bash
source <workspace>/alp-env.sh   # ZEPHYR_BASE, toolchain
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
    examples/aen/aen-evk-demo -- \
    "-DEXTRA_ZEPHYR_MODULES=<path-to-this-alp-sdk-checkout>;<path-to-hal_alif>"
```

Confirmed building clean against this tree:

```
Memory region         Used Size  Region Size  %age Used
           FLASH:      143296 B       256 KB     54.66%
             RAM:       60504 B       256 KB     23.08%
           SRAM0:         14 KB         4 MB      0.34%
```

`SRAM0` holds phase 13's two JPEG buffers (6144 B source + 8192 B output).
They live there and not in `RAM` because the Hantro block is an AXI bus
master that cannot reach the M55's core-local DTCM -- see phase 13's
comment block in `src/main.c`.

`RAM` is mostly phase 8: `sizeof(cc3501e_t)` is ~32 KB (the driver keeps its
tx/rx scratch, scan and socket buffers inside the handle), which is why that
handle is **file-static and not a stack local** -- as a local it crosses
`PSPLIM` and the M55 raises `STKOF` -> UsageFault before a line is printed.

## Console

This bench's app UART emits nothing under the Flow C RAM-run this app
targets and exports no SE-UART. `prj.conf` carries the RAM-console toggle
every sibling AEN bench app documents -- comment the four `UART_CONSOLE`
lines and uncomment the `RAM_CONSOLE` pair to read `ram_console_buf` over
SWD instead.

## Expected output shape

Phase 8's own lines, which carry the whole diagnostic for the coprocessor
(a bench run costs a reservation, so nothing is left to be inferred):

```
[evkdemo] -- Phase: CC3501E Wi-Fi 6 / BLE 5.4 (inter-chip SPI1 bridge) --
[evkdemo] CC3501E: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ 25000000 Hz) -> 0
[evkdemo] CC3501E: PING (0x00) -> 0 after 1 attempt(s) of 25 (200 ms apart)
[evkdemo] CC3501E: GET_VERSION (0x01) -> 0 protocol v1.0 (host built for v1.0) match
[evkdemo] CC3501E: GET_MAC (0x03) -> 0  44:3e:8a:10:b6:9e  OUI=44:3e:8a  plausible station MAC
[evkdemo] CC3501E: GET_CAPABILITIES (0x06) -> 0 caps=0x00000fff wifi_sta=yes ble=yes
[evkdemo] CC3501E:   scan[0] "example-ap" ch6 -52 dBm wpa2 bssid=.. ok
[evkdemo] CC3501E: WIFI_SCAN_START (0x10) -> 0  7 network(s) seen, 7 plausible  ok
[evkdemo] CC3501E: BLE_ENABLE (0x30) -> 0 (controller + NimBLE host up; left ENABLED for later phases)
[evkdemo] CC3501E: ping=ok version=ok mac=ok caps=ok scan=ok ble=ok -> PASS
```

The MAC, SSIDs and counts above are **shape, not expected values** -- the
protocol version and the `44:3e:8a` OUI (MA-L Texas Instruments; the CC3501E
carries a TI factory MAC and Alp Lab holds no IEEE OUI) are the only parts a
reader should treat as characteristic.

The summary:

```
[evkdemo] phase  1/14: RTC + temperature (BRD_I2C)         PASS
[evkdemo] phase  2/14: Sensors (BMI323/ICM42670/BMP581)     PASS
[evkdemo] phase  3/14: Power rails (6x INA236)              PASS
[evkdemo] phase  4/14: I/O expander answers (TCAL9538, read-only) PASS
[evkdemo] phase  5/14: EEPROM identity (24C128)             PASS
[evkdemo] phase  6/14: RGB LED (PWM0/1/3)                   PASS
[evkdemo] phase  7/14: Rotary encoder                       SKIPPED
[evkdemo] phase  8/14: CC3501E Wi-Fi/BLE                    PASS
[evkdemo] phase  9/14: SD card                              SKIPPED
[evkdemo] phase 10/14: Ethernet                             SKIPPED
[evkdemo] phase 11/14: Sound out -> PDM in                  SKIPPED
[evkdemo] phase 12/14: Screen (DSI)                         SKIPPED
[evkdemo] phase 13/14: JPEG encode (Hantro VC9000E)         PASS
[evkdemo] phase 14/14: NPU inference                        SKIPPED
...
[evkdemo] RESULT: 8 PASS, 6 SKIPPED, 0 FAIL
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
- [`examples/aen/aen-jpeg-regcheck`](../aen-jpeg-regcheck/) -- the
  silicon-proven JPEG reference phase 13 follows: its file header records
  the three defects a real bench run exposed (the missing SoC select that
  lets the software fallback win, DMA buffers landing in unreachable DTCM,
  and `#ifdef`-ing the source layout instead of querying
  `alp_jpeg_caps_t::pixfmt_mask`).
- [`examples/aen/aen-cc3501e-bringup`](../aen-cc3501e-bringup/) -- the
  silicon-proven host-side CC3501E bring-up phase 8 follows: its file header
  documents the wiring, the host-gated supply, the hardware-SS0 chip-select
  model and the per-phase READY gating. `src/cc3501e_bridge.{c,h}` is copied
  from it byte-for-byte (that pair is the SoM bring-up *template* every AEN
  app copies, not a library).
- [`examples/aen/aen-cc3501e-companion-tour`](../aen-cc3501e-companion-tour/)
  and [`aen-cc3501e-ble-gatt`](../aen-cc3501e-ble-gatt/) -- the call shapes
  for the wider Wi-Fi/socket and BLE/GATT surfaces phase 8 deliberately does
  not reach into.
- [`examples/aen/aen-npu-inference-alp`](../aen-npu-inference-alp/) -- the
  NPU path phase 14 stubs out, and the reason it does.
- [`<alp/boards/alp_e1m_evk_routes.h>`](../../../include/alp/boards/alp_e1m_evk_routes.h)
  -- `EVK_I2C_ADDR_*` / `EVK_PWM_LED_*` map.
