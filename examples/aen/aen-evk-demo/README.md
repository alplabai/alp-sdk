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

Fourteen phases run in a fixed order. Nine are fully implemented against
bench-proven drivers -- the first six, plus phase 8 (the CC3501E Wi-Fi 6 /
BLE 5.4 coprocessor over the inter-chip SPI bridge), phase 10 (RMII Ethernet
through the GMAC and the on-module DP83825 PHY) and phase 13 (JPEG encode on
the Hantro VC9000E); the remaining five are stubs that always
report `SKIPPED`, each with its own reason (see `src/main.c`'s "STUBS" section)
-- an attended-run requirement, a larger deferred unit of work, "no panel
on this bench", and a model that would need a different **boot flow** are
different kinds of gaps and are described as such, not collapsed into one
generic "not implemented".

Phase 10 also changed the image's **memory map** for every other phase -- see
[Memory map](#memory-map) before touching anything about placement.

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
| 8 | CC3501E Wi-Fi/BLE | inter-chip SPI1 (P14_6/5/4 + hardware SS0 P14_7), WIFI_EN P15_5, nRESET P15_1_FLEX, READY P2_6 | Powers (`WIFI_EN` high) and resets the coprocessor -- **nothing answers before this**, its supply is host-gated -- then `PING` (`0x00`) with a bounded 25 x 200 ms retry, `GET_VERSION` (`0x01`) **compared on `ALP_CC3501E_PROTOCOL_MAJOR` only** (per ADR 0033 a MINOR delta is additive and safe -- it is reported, not gated), `GET_MAC` (`0x03`) checked for a structurally valid station address, `GET_CAPABILITIES` (`0x06`), a passive `WIFI_SCAN_START` (`0x10`), and `BLE_ENABLE` (`0x30`). Every return code is printed. `PASS` requires **all five** of major-version match, valid MAC, capabilities readable, scan round-tripped, BLE up -- a `PING` alone is explicitly not enough. | Signal quality, throughput, or that any network is reachable -- it never associates. An **empty scan is `PASS`-but-`UNCORROBORATED`**: zero networks is a statement about the RF environment, not about this board, so the gate is that the scan *round-tripped*, not that it found anything. Which colour of failure a dead link is (power / pinmux / firmware) -- the log names the three to check. |
| 9 | SD card | -- | Nothing (stub). | Two separate blockers, only one of them software. **No card is fitted** on the bench this app is developed against, so no round trip can be exercised. And **the mux SELECT is not software-drivable on r2**: the microSD sits behind a 74LVC157 pair with ENABLE on E1M `IO20` (CC3501E `GPIO_26`, routed on both revisions) and SELECT on E1M `IO21`, which reached CC3501E `GPIO_30` on r1 but was left open on the module for r2. Set it on header **P18** instead -- a jumper pulls `MUX_SEL.SDIO` high through `R198`, an open header lets `R27` pull it to `0V`, and that net drives both mux selects (`U38.S`, `U39.S`). **On r1 do not fit the jumper while firmware drives `IO21`** -- the net reaches both P18 and E2 `L3`. Beyond a card and a mux position, a real phase still needs the SDIO host bring-up and the ENABLE sequence over the proxy (`CONFIG_ALP_SDK_GPIO_CC3501E_PROXY` plus a route table this app does not carry). |
| 10 | Ethernet | RMII GMAC `ethernet@48100000` + on-module TI DP83825 PHY (REFCLK P11_0, RXD0 P11_3, RXD1 P1_1, CRS_DV P6_7, TXD0 P10_4, TXD1 P10_5, TXEN P1_5, MDC/MDIO P11_2/P11_1); PHY power `E_PHY_PWRDWN` P15_4, reset `E_PHY_RESET` P11_6 | Powers and resets the PHY from a `SYS_INIT` hook that runs **before the Ethernet driver's own init** -- the RMII ref-clock AUTO probe only finds the module's external 50 MHz oscillator if the PHY is already powered when it runs, so the phase reports which source `ETH_CTRL` bit 4 latched. Then reads the PHY's real status **over MDIO** (fixed-link does no MDIO of its own), sets `RCSR` bit 7 for 50 MHz-reference RMII, restarts auto-negotiation, and gates `PASS` on **a DHCPv4 lease** -- DISCOVER/OFFER/REQUEST/ACK completes only over a genuinely bidirectional link. Prints `tx_bytes`/`rx_bytes` throughout. | That `net_if_is_carrier_ok()` means anything: with an unmanaged fixed-link PHY it is **synthetic**, it reports what devicetree hard-codes, and it reads true with the cable in your hand. It is printed labelled as such and is never gated on. Also: link speed/throughput, and whether a segment that sends us nothing is quiet or broken -- see the verdict table below. |
| 11 | Sound out -> PDM in | -- | Nothing (stub). | I2S bring-up + the low-volume ramp policy for the ~15 W class-D amps deferred to the next slice. |
| 12 | Screen (DSI) | -- | Nothing (stub). | No panel on this bench; a clean DSI init would not prove one is attached anyway. |
| 13 | JPEG encode | Hantro VC9000E @ `0x49044000` (`jpeg0`) | Encodes a synthetic 64x64 NV12 gradient through `<alp/jpeg.h>`. Prints which backend won (`caps.hw_accelerated`) and **fails a software-fallback win** -- on this board the hardware encoder is the phase. Asserts the output really is a JPEG: SOI `FF D8 FF` at the start, EOI `FF D9` at the end, and a plausible length (>= 256 B, < the 6144 B source). Every return code is printed verbatim. | The image is *correct* -- the checks are structural, not a decode. The Hantro hardware-ID readback: `<alp/jpeg.h>` exposes no accessor for `JPEG_SWREG0`, and this example will not hand-roll a register poke. A mismatch against `JPEG_HW_ID` (`0x90001000`) still surfaces, as `alp_jpeg_open() == NULL` with `ALP_ERR_NOT_READY` plus the driver's own `"JPEG hardware not found (ID: 0x%08x)"` `LOG_ERR` line (this app builds `CONFIG_LOG=y`). |
| 14 | NPU inference | -- | Nothing (stub). | **A boot-flow change, not a phase.** `aen-npu-inference-alp` is the silicon-proven Ethos-U85 path through `<alp/inference.h>`, but its Vela-compiled `person_detect_u85` model is **~263 KiB** -- which is exactly why that app links into MRAM slot0 and boots via Flow D. This demo is a **Flow C ITCM RAM-run**: ITCM is **256 KB total** and the demo now uses roughly two thirds of it -- phase 8's CC3501E bridge driver and phase 10's network stack were each worth tens of kilobytes, so the headroom is shrinking, not growing. The model does not fit alongside it, so adding NPU here means relinking the whole demo into MRAM slot0. Shrinking the model to fit would swap a proven artefact for an unproven one. |

### Phase 10's verdicts, and why "no cable" is not a failure

A DHCP lease is the only honest `PASS` gate here, but it needs a switch with a
DHCP server on the far end, which an arbitrary bench run cannot be assumed to
have. The phase asks the PHY over MDIO what the wire is doing, and then asks
the interface's own byte counters what actually moved:

| Observation | Verdict | Why |
|---|---|---|
| No PHY answers on any of the 32 MDIO addresses | `FAIL` | The DP83825 is fitted on **every** E1M-AEN SoM. Silent means unpowered, unclocked or unreset -- our hardware, not the operator's cable. |
| PHY answers, auto-negotiation never completes | `SKIPPED` | Nobody on the other end. This is "is a cable plugged in?", the normal state of an unattended bench. |
| Link up, but the MAC transmitted **zero bytes** | `FAIL` | DHCP queued DISCOVERs and the link is up, so no frame left the part whatever is out there. Unambiguous, and the TX half of the DMA-placement failure the overlay's memory block exists to prevent. |
| Link up, TX moved, no lease | `SKIPPED` + qualifier | Either no DHCP server on this segment or a dead RX path. `rx_bytes` separates them in the log and the qualifier carries it into the summary table -- but a live switch port with no other talkers legitimately sends us nothing, so failing on it would be the mirror-image lie. |
| Link up **and** a lease acquired | `PASS` | DISCOVER/OFFER/REQUEST/ACK completes only over a genuinely bidirectional link. |

`carrier_ok` appears in none of those rows on purpose. This app exists because
an earlier example counted a successful ID read as a pass; a synthetic carrier
bit is exactly that shape of claim.

## Memory map

**Phase 10 moved the whole image's system RAM out of the M55 DTCM into the
global on-chip SRAM0 bank.** This affects every phase, so it is documented
here as well as in full in the app overlay's header.

The GMAC is a DMA bus master and the upstream DWMAC core hands it the raw CPU
pointer -- no address translation. The M55 DTCM is tightly-coupled and **not**
on the GMAC's AXI path, so descriptor rings and `net_buf`s left there are
invisible to it and **zero frames move in either direction even with the wire
link up** (bench-observed on both sides at once by `aen-ethernet-link`, which
calls this its decisive fix). No narrower fix is available to an application:
the rings are file-static in the Ethernet driver and the pool is file-static in
the net subsystem, so neither can be section-tagged the way phase 13's JPEG
buffers are -- and the driver's own header says the requirement is enforced
"at the board/SoC layer, not in this glue".

But this app, unlike `aen-ethernet-link`, **already** puts buffers in the
`SRAM0` linker region. The SoC's `sram0` node is both a `zephyr,memory-region`
(emitting the linker region phase 13's buffers land in) and, if chosen, the
source of the main RAM region. Choosing it plainly makes both start at
`0x02000000`; the linker then allocates into them independently **and does not
warn**. Measured on this tree, `.data`/`.bss`/`.noinit` landed on top of
`jpeg_out` and `jpeg_src` -- phase 13 writing its gradient would have scribbled
over the whole image's static state, silently.

So the overlay gives the two consumers disjoint windows of the same bank:

| Window | Contents |
|---|---|
| `0x0200_0000` + 64 KiB | The `SRAM0` linker region: phase 13's two JPEG buffers, at the same addresses they had before phase 10 existed. |
| `0x0201_0000` + 512 KiB | System RAM: `.data`/`.bss`/`.noinit`, every stack, the GMAC descriptor rings and the `net_buf` pool. |
| above that | Unused remainder of the 4 MiB bank. |

Shrinking `&sram0`'s `reg` to that 64 KiB window is the safety property, not a
tidy-up: it turns a future oversized `SRAM0`-tagged buffer into a **link error**
instead of a silent walk into system RAM.

**What it costs the other phases**, stated plainly because it cannot be checked
off the bench: `CONFIG_DCACHE` is off on this silicon, so there is no cache to
soften the move -- data that used to hit single-cycle DTCM now goes to uncached
global SRAM over the fabric. Nothing becomes incorrect; things become slower.
The one phase with an inner loop tight enough to care is **phase 8**, whose
SPI1 FIFO refill feeds a 25 MHz link and whose DW-SSI master deasserts its own
chip-select if it underruns mid-frame. That is a hypothesis, not a measurement.

**If phase 8 starts failing after this change, read it as a memory-placement
regression first, not a CC3501E fault.** Phase 8 does print its own verdict, so
a hard regression is visible — but an intermittent underrun costing one frame
in fifty presents as a *flaky* phase 8 with nothing pointing at phase 10, and
the bridge has enough genuine failure modes of its own (power, pinmux, the
`READY` re-arm race, `RX_SAMPLE_DLY`) to absorb the blame. The cheap
disambiguation is to rebuild with `zephyr,sram` back on `&dtcm` and phase 10
dropped; if phase 8 goes solid, it is this memory map.

The pre-change baseline, measured on silicon, is what a good phase 8 looks like:

```
CC3501E: bridge bring-up (WIFI_EN high, nRESET pulsed, SPI1 @ 25000000 Hz) -> 0
CC3501E: PING (0x00) -> 0 after 1 attempt(s) of 25 (200 ms apart)
CC3501E: GET_VERSION (0x01) -> 0 protocol v3.1 (host built for v3.1) match
CC3501E: GET_MAC (0x03) -> 0  44:3e:8a:10:b6:a7
CC3501E: GET_CAPABILITIES (0x06) -> 0 caps=0x00000fff
CC3501E: WIFI_SCAN_START (0x10) -> 0  4 network(s) seen, 4 plausible  ok
CC3501E: BLE_ENABLE (0x30) -> 0
```

`PING` needing more than one attempt, or a `GET_MAC` that comes back shifted by
a leading `0x00`, are the two symptoms that would point at timing rather than
at the coprocessor.

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

Two further buses are **not** shared through `demo_ctx_t`:

- **RMII + MDIO** -- phase 10's Ethernet route to the on-module DP83825 PHY.
  These *are* published by `metadata/e1m_modules/aen/alif-ethernet-phy.tsv`
  (the authoritative SoM route -- **not** the Alif fork's reference route, which
  an earlier cut of `aen-ethernet-link` wrongly used), but the `ethernet` node
  ships `status = "disabled"` in the SoC dtsi, so this app's overlay enables it
  and supplies the pin group. Phase 10 owns it end to end.
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

Builds clean against this tree. Roughly two thirds of the 256 KB ITCM `FLASH`
region is used, and well under a fifth of the 512 KiB system-RAM window; check
the linker's own summary rather than a figure written down here, which is why
no byte count or percentage is quoted (two of them have already gone stale).

`SRAM0` holds phase 13's two JPEG buffers (6144 B source + 8192 B output).
They live in their own linker region and not in system RAM because the Hantro
block is an AXI bus master that cannot reach the M55's core-local DTCM -- see
phase 13's comment block in `src/main.c`, and [Memory map](#memory-map) for why
that region and system RAM now need explicitly disjoint windows.

System RAM is mostly phase 8: `sizeof(cc3501e_t)` is ~32 KB (the driver keeps
its tx/rx scratch, scan and socket buffers inside the handle), which is why that
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
[evkdemo] CC3501E: GET_VERSION (0x01) -> 0 protocol v3.1 (host built for v3.1) match
[evkdemo] CC3501E: GET_MAC (0x03) -> 0  44:3e:8a:xx:xx:xx  OUI=44:3e:8a  plausible station MAC
[evkdemo] CC3501E: GET_CAPABILITIES (0x06) -> 0 caps=0x00000fff wifi_sta=yes ble=yes
[evkdemo] CC3501E:   scan[0] "example-ap" ch6 -52 dBm wpa2 bssid=.. ok
[evkdemo] CC3501E: WIFI_SCAN_START (0x10) -> 0  7 network(s) seen, 7 plausible  ok
[evkdemo] CC3501E: BLE_ENABLE (0x30) -> 0 (controller + NimBLE host up; left ENABLED for later phases)
[evkdemo] CC3501E: ping=ok version=ok mac=ok caps=ok scan=ok ble=ok -> PASS
```

The device-specific octets, SSIDs and counts above are **shape, not expected
values** -- the protocol version and the `44:3e:8a` OUI (MA-L Texas
Instruments; the CC3501E carries a TI factory MAC and Alp Lab holds no IEEE
OUI) are the only parts a reader should treat as characteristic.

A `v3.0` firmware against this `v3.1` host would print `major match, minor
differs` and still `PASS` -- ADR 0033 defines MINOR as additive, so the link
is compatible; ask `GET_CAPABILITIES` about features rather than the version.
Only a MAJOR skew fails, and it fails earlier: `cc3501e_reset()` refuses it
during bring-up, so the phase short-circuits with the firmware's own
major/minor rather than burning the full PING retry budget against a handle
the driver has already marked down.

Phase 10's own lines, on a bench where a cable IS plugged into a switch with a
DHCP server:

```
[evkdemo] -- Phase: Ethernet (RMII GMAC + on-module DP83825 PHY) --
[evkdemo] ETH: MAC 02:01:56:xx:xx:xx (per-boot random locally-administered, from the SoC dtsi's zephyr,random-mac-address)
[evkdemo] ETH: RMII refclk = EXTERNAL oscillator -- the PHY was powered before the probe (ETH_CTRL bit4=0)
[evkdemo] ETH: net_if_up -> 0
[evkdemo] ETH: MDIO PHY@0 id=2000a140 (DP83825 = 2000a140)
[evkdemo] ETH: PHY regs ANAR=01e1 ANLPAR=45e1 PHYSTS=0000 RCSR=0001
[evkdemo] ETH: RCSR 0x0001 -> 0x0081 (REF_CLK_SEL = 50 MHz reference)
[evkdemo] ETH: wire link UP after 2500 ms (BMSR=786d ANLPAR=45e1)
[evkdemo] ETH: admin_up=1 carrier_ok=1(SYNTHETIC, not a link proof) tx_bytes=1188 rx_bytes=684 dhcp_bound=1
[evkdemo] ETH: DHCP lease = 192.168.10.xxx -- wire link UP and both DMA directions proven end to end
```

The addresses, register values and byte counts are **shape, not expected
values**; the DP83825 identity `2000a140` and `ETH_CTRL bit4=0` are the two
parts a reader should treat as characteristic. Note the ordering: the `PHY
regs` line is read *before* `RCSR` bit 7 is set, so it shows the pre-write
value and the next line shows the write -- a transcript where both read `0081`
is one that has been edited. On a bench with nothing plugged
in, the same phase stops after `wire link DOWN` and reports
`SKIPPED -- no carrier -- cable?`, which is a normal unattended run and not a
failure.

The summary, from a run with a cable and a DHCP server:

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
[evkdemo] phase 10/14: Ethernet                             PASS
[evkdemo] phase 11/14: Sound out -> PDM in                  SKIPPED
[evkdemo] phase 12/14: Screen (DSI)                         SKIPPED
[evkdemo] phase 13/14: JPEG encode (Hantro VC9000E)         PASS
[evkdemo] phase 14/14: NPU inference                        SKIPPED
...
[evkdemo] RESULT: 9 PASS, 5 SKIPPED, 0 FAIL
[evkdemo] done
```

With no cable in the port, phase 10 reads
`Ethernet   SKIPPED  no carrier -- cable?` and the tally is
`8 PASS, 6 SKIPPED, 0 FAIL` -- still not a failed run.

A run with skips is not a failed run -- the three counts are always
reported together.

A phase may attach a one-line **qualifier** to its own verdict, printed after
it on both the per-phase line and in the summary table. A qualifier explains a
verdict, it is never a fourth one and never changes what a phase counts as.
Phase 8 sets one when its Wi-Fi scan comes back empty --
`CC3501E Wi-Fi/BLE   PASS    scan UNCORROBORATED -- 0 networks seen` -- and
phase 10 sets one on **every** non-`PASS` outcome, `SKIPPED` and `FAIL` alike.
That matters most for the `FAIL`s, which are otherwise indistinguishable in the
table: `Ethernet  FAIL  no PHY answered on MDIO` and
`Ethernet  FAIL  link UP but the MAC transmitted ZERO bytes` are different
bench sessions, and so are
`Ethernet  SKIPPED  no carrier -- cable?` and
`Ethernet  SKIPPED  link UP, TX ok, RX silent -- no DHCP server, or dead RX`.

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
- [`examples/aen/aen-ethernet-link`](../aen-ethernet-link/) -- the
  bench-verified Ethernet reference phase 10 follows (a real DHCP lease off the
  bench switch, server-side reachable). Its file header records the three
  things that had to be right and in what order: PHY power **before** the
  driver's ref-clock probe, `RCSR` bit 7 for 50 MHz-reference RMII, and the
  DMA buffers off the DTCM -- the last of which is why this app's memory map
  looks the way it does.
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
