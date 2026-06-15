# Bench bring-up — E1M-AEN

Step-by-step procedure for bringing a freshly-assembled E1M-AEN
module up on the bench.  Assumes you have an `E1M-EVK` board
(or a pin-compatible custom board), a SWD debug probe (J-Link
or ST-Link), an external 5 V bench supply, and a USB-UART
adapter.

> Peer docs: [`bring-up-v2n.md`](bring-up-v2n.md),
> [`bring-up-v2n-m1.md`](bring-up-v2n-m1.md),
> [`bring-up-imx93.md`](bring-up-imx93.md).  This guide covers the
> AEN family specifically (AEN301..801, Alif Ensemble silicon).

## 0. Pre-flight

Inventory check before powering anything:

* Module populated (the **lead part** is the **E1M-AEN801 / Alif
  Ensemble E8** -- per [`E1M-AEN801.yaml`](../metadata/e1m_modules/E1M-AEN801.yaml)
  `on_module:`):
  - **Alif Ensemble E8** SoC (2x Cortex-A32 + 2x Cortex-M55
    (HP + HE) + Ethos-U85 + dual Ethos-U55).  The broader
    **E3..E8** family scales cores + NPU count + memory down from
    this; lower SKUs drop the A32 cluster and/or NPUs.
  - **24C128** EEPROM with the 128-byte Alp manifest at offset
    `0x0000`.
  - **CC3501E** Wi-Fi 6 + BLE coprocessor (TI SimpleLink).
  - **OPTIGA Trust M** secure element.
  - **RV-3028-C7** external RTC.
  - **TMP112** temperature sensor.
  - **DP83825I** 10/100 Ethernet PHY (a single MAC; ET1 is
    E1M-X-only) -- see [`docs/soms/aen.md`](soms/aen.md).
* Board populated (**Alp E1M-EVK** carrier -- per
  [`metadata/boards/e1m-evk.yaml`](../metadata/boards/e1m-evk.yaml)
  `populated:`): E1M-edge passthroughs + the 5 V power input +
  JTAG/SWD header + USB-UART for console, plus the carrier's
  soldered parts: **TCAL9538** GPIO expander, **6x INA236** power
  monitors, **BMP581** barometer, **CAM_MUX_PI3WVR626** MIPI CSI
  2:1 camera mux, BMI323 + ICM-42670 IMUs, TAS2563 amps, PDM mics.
  (Carrier parts ride **BRD_I2C** / the EVK headers, not the SoM.)

> **This batch: the SoM's OSPI memories are NOT populated.** The
> OSPI0 octal bus (BOM-optional NOR flash on CS0 + HyperRAM on CS1,
> both `assembled: optional` in the SKU preset) is un-stuffed on the
> AEN801 modules on the bench, so boot **and** app storage run from
> on-die **MRAM only** (5.5 MB on `AE822FA0E5597LS0`).  Don't expect
> an external flash / XIP device on this hardware; MCUboot slots and
> any storage partition must target MRAM, not OSPI.

## 1. First-power smoke test

1. Connect a current-limited bench supply (1 A limit) to V_IN.
2. Power on; watch the supply.  Steady-state current should be
   **~80..150 mA** with the SoC idle and the Ethos-U55 in clock-
   gated state.
3. Probe the on-module 3.3 V rail (V_3V3) at the test-point.
   Within spec: 3.30 V ±2 %.
4. Probe V_CORE (the M55 core rail, ~0.9 V on most AEN SKUs).
   Within spec: 0.90 V ±3 %.

If V_CORE is missing the Alif PMIC sequencer didn't release the
core rail.  Check the EN strap on the board first, then the
PMIC's `EVENT_00` status register over BRD_I2C.

## 2. SWD probe attach

1. Wire the SWD probe to the board's debug header
   (SWDIO/SWCLK/nRST/GND).  Power the board (probe stays
   unpowered; standard 1.8/3.3 V level convention applies).
2. With the probe plugged in, attach with J-Link Commander. **Use the
   generic `Cortex-M55` device, _not_ the Alif part number** — on the E8
   bench the part-specific device (`AE822FA0E5597LS0_M55_HE`) connect
   sequence fails post-boot ("Could not connect to the target device"),
   while the generic core device scans the APs and finds the core
   directly (BENCH-VERIFIED on the E1M-AEN801, 2026-06-15):

   ```bash
   JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1
   ```

   The SW-DP IDR (the debug-port identification register — a property of
   the ADIv5 SW-DP, **not** a core ID) reads **`0x4C013477`** on the E8
   (BENCH-VERIFIED). Note this is *not* the generic `0x6BA02477` this repo
   reads for the GD32/Cortex-M33 — a wrong value means wrong target or
   reversed SWD wiring.

   > pyocd works too, but its `-t` target id depends on the installed
   > `alif_ensemble-cmsis-dfp` CMSIS-pack (do NOT assume an `alif_e8` id).

3. The attach enumerates the core and reads `CPUID`:
   **`0x411FD220`** (BENCH-VERIFIED) → ARM (impl `0x41`), Cortex-M55
   **r1p0**. (The earlier `0x410FD220` guess was r0p0 — wrong revision
   nibble; the silicon is r1p0.) J-Link also reports `Secure debug:
   enabled` and the full CoreSight ROM table (DWT/FPB/ITM/ETM/CTI).

   > A fresh, un-provisioned SoM finds **no core** here ("Could not find
   > core in CoreSight setup") — the SES holds the M55 until an app is
   > provisioned. If you see that, the board isn't broken: provision an
   > app first (see [`aen-provisioning.md`](aen-provisioning.md)), then
   > the debug-AP comes alive as above.

## 3. EEPROM manifest read

The 24C128 carries the 128-byte Alp manifest at offset
`0x0000`.  Read it back through the board's BRD_I2C bus:

```bash
i2cdetect -y 1                 # confirm 0x50 ACKs
i2cdump  -y 1 0x50 b            # full 128-byte hexdump
```

Cross-reference against
[`include/alp/hw_info.h`](../include/alp/hw_info.h):

* Bytes 0..3:   magic `ALPH` (`0x41 0x4C 0x50 0x48`)
* Byte 4:       `schema_v1` = `0x01`
* Bytes 5..8:   family ASCII = `AEN_`
* Bytes 9..24:  SKU ASCII (`E1M-AEN701` zero-padded)
* Bytes 25..32: hw_rev ASCII (`r1` zero-padded)
* Bytes 33..48: serial number ASCII
* Bytes 49..56: mfg_date BCD (`YYYYMMDD`)
* Bytes 57..123: reserved (zero)
* Bytes 124..127: CRC32 (little-endian) over bytes 0..123

If the manifest is unprogrammed, run
[`scripts/program_eeprom.py`](../scripts/program_eeprom.py)
against the SKU's `som.yaml` preset.

## 4. Console + first boot

1. Wire USB-UART to UART0 on the board (silkscreen
   `USB_UART_TXD` / `_RXD`).  Standard 115200 8N1.
2. Open a terminal (minicom, screen, picocom).
3. Power-cycle the board.  Expect the boot ROM banner on the
   UART within ~200 ms:

   ```
   AlifSemi BootROM v1.x.y
   Stage1 ...
   ```

   No boot ROM output usually means: wrong UART selected (TX/RX
   swapped), wrong baud, or the PMIC didn't release the core.

4. An Alp-Lab-provisioned module ships with a dev-signed **MCUboot**
   (the SES-launched ATOC) plus a **self-test** image in slot0, so it
   boots that on power-up and the M55 core is already released — `west
   flash`/SWD work directly.  (A *bare* module sourced outside Alp Lab,
   or one whose ATOC was wiped, reports `No ATOC` and the core stays
   gated until you provision MCUboot over the SE-UART — see
   [`aen-provisioning.md`](aen-provisioning.md).)  To take it over with
   your own image, build with sysbuild so MCUboot signs it into slot0:

   ```bash
   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he \
       examples/peripheral-io/gpio-button-led \
       --sysbuild --sysbuild-config zephyr/sysbuild/aen/sysbuild.conf
   west flash
   ```

   Expected output on the UART: the
   `[gpio] init button=BOARD_PIN_ENCODER_SW, led=BOARD_PIN_LED_RED` banner
   from the SDK's first-build tutorial.

## 5. Peripheral sanity checks

Run these in order; each one exercises a different on-module
or on-board subsystem.

### 5.1 BRD_I2C: probe every on-module slave

The AEN module routes one shared BRD_I2C bus.  Drive an
i2cdetect from a built `i2c-scanner` example or via the
console:

| Slave | 7-bit addr | What | Where |
|-------|------------|------|-------|
| 24C128 | `0x50` | EEPROM (manifest) | SoM |
| OPTIGA TM | `0x30` | Secure element | SoM |
| RV-3028-C7 | `0x52` | RTC | SoM |
| TMP112 | `0x48` | Thermometer | SoM |
| TCAL9538 | `0x72` | GPIO expander | EVK carrier |
| INA236 | `0x40`..`0x46` | Power monitor (6x) | EVK carrier |
| BMP581 | `0x47` | Barometer | EVK carrier |

A missing slave that's *expected* is a real fault.  The on-module
set is authoritative in
[`E1M-AEN801.yaml`](../metadata/e1m_modules/E1M-AEN801.yaml)'s
scalar `on_module:` keys (the AEN presets have no `i2c_devices:`
block -- that's a V2N/V2M-only convention); the carrier-side parts
are authoritative in
[`metadata/boards/e1m-evk.yaml`](../metadata/boards/e1m-evk.yaml)'s
`populated:` block, where individual parts can be flipped off for
DNI variants.

### 5.2 OPTIGA Trust M sanity

The AEN secure-element example
([`examples/aen/aen-secure-element-sign`](../examples/aen/aen-secure-element-sign))
exercises the OPTIGA Trust M over the portable `<alp/*>` API on
BRD_I2C.  (It is the AEN sibling of the V2N variant -- identical
`src/`, AEN `board.yaml` with `m55_he` as the BRD_I2C owner.)

```bash
west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/aen-secure-element-sign
west flash
```

Expect a successful OPTIGA read/sign on the console.  An
all-zeros / all-FFs device ID or a probe failure means the
OPTIGA wasn't bonded out correctly on this assembly.  (Running on
hardware needs BRD_I2C / LPI2C0 wired to portable bus 0 -- the
alp-sdk Alif LPI2C bring-up.)

### 5.3 Ethernet PHY link

Connect a 1 Gb link partner to the board's RJ45.  Reset the
module and check link-up via the DP83825I's MDIO registers:

```c
uint16_t bmsr = 0;
mdio_read(0x01, &bmsr);    // PHYAD usually 0x01 on AEN
// bit 2 = link up; bit 5 = autoneg complete
```

100/full-duplex on real silicon should come up within ~500 ms
of cable insert.

### 5.4 Camera + display

These are optional and SKU-/carrier-dependent.  The E1M-EVK
carries a `CAM_MUX_PI3WVR626` MIPI CSI 2:1 mux (selected via
`EVK_PIN_CAM_MUX_SEL`), but no camera-mux truth table is published
yet -- treat the wiring as TBD until the carrier camera doc lands.

## 6. Bench-day bring-up runbook (first physical SoM)

When the first physical AEN SoM lands on the bench, run this
ordered checklist top-to-bottom.  Each step is a hard gate: do
**not** advance until the prior one passes.  §1..§5 above carry
the detailed wiring + register tables -- this section is the
one-page sequence that ties them together, plus the three steps
(mailbox controller, Ethos-U, model load) that bring-up adds on
top of the per-subsystem checks.

> Board target: **the first SoM to arrive is the E1M-AEN801 (E8).**
> AEN701 (E7) is deprioritised and may never be produced -- AEN801
> supersedes it (it adds the Ethos-U85 on top of the U55 pair), so
> bench bring-up centres on the **E8** part.  E8 is fully supported on
> **alp-sdk's own upstream Zephyr base (v4.4.0)** -- no fork needed,
> backed by the `hal_alif` module pinned in our `west.yml`.
>
> The **primary target is the Alp-Lab carrier board**, now authored +
> build-verified in-repo (`boards/alp/e1m_aen801_m55_{he,hp}`):
> **`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`** (M55-HE) /
> **`…/rtss_hp`** (M55-HP).  It carries the carrier-accurate E1M-EVK
> peripheral wiring (console on UART5, the MRAM MCUboot partition map,
> the alp-sdk Alif drivers) and is what the commands below use.  It is
> build-verified but **not yet bench-booted** for a Zephyr image -- the
> SES provisioning + Alif stock-blink boot is proven (§4,
> [`aen-provisioning.md`](aen-provisioning.md)); booting our own Zephyr
> image on it is the next bench step.
>
> **Upstream known-good fallback:** `boards/alif/ensemble_e8_dk` ships
> the exact part, so `ensemble_e8_dk/ae822fa0e5597ls0/rtss_{he,hp}` is a
> guaranteed-buildable reference target -- use it to prove the
> toolchain + silicon before switching to the carrier board.  (Alif's
> own `sdk-alif` / `zephyr_alif` fork -- board `alif_e8_dk` -- and the
> CMSIS-Pack DFP (`alif_ensemble-cmsis-dfp`, device `AE822FA0E5597`)
> are opt-in alternatives; Yocto/A32 is `meta-alif-ensemble` branch
> **scarthgap**, `devkit-e8.conf` / `appkit-e8.conf`.  Note E7 is not in
> upstream Zephyr v4.4 at all -- only e4/e6/e8/e1c -- another reason E8
> leads.)
>
> Per-core builds use plain `west build -b <target> <app>`.
> (`west alp-build <app>` is the multi-core *orchestrator*: it fans a
> board.yaml out into per-core slices using the SoM-preset board string,
> which resolves to the `alp_e1m_aen801_m55_{he,hp}` carrier board --
> **prefer it** once the carrier board boots, as it builds both M55
> cores from the example's board.yaml with the EVK's actual routing.)

0. **Current-limited power-on + rail check.**  Bench supply at a
   **1 A** limit on V_IN (§1).  Power on, watch steady-state
   current settle to **~80..150 mA**, then probe V_3V3
   (3.30 V ±2 %) and V_CORE (~0.90 V ±3 %).  A current trip or a
   missing rail stops here -- see §1's PMIC-sequencer note.

1. **SWD/J-Link attach + CPUID read.**  Wire the probe (§2),
   then (use the **generic `Cortex-M55` device**, not the Alif part
   number — the part-specific device connect fails post-boot):

   ```bash
   JLinkExe -device Cortex-M55 -if SWD -speed 4000 -nogui 1
   ```

   SW-DP IDR (debug-port ID, **not** a core ID) reads **`0x4C013477`**
   and `CPUID` reads **`0x411FD220`** (Cortex-M55 **r1p0**) — both
   BENCH-VERIFIED on the E1M-AEN801 (2026-06-15).  Wrong values =
   wrong target or reversed SWD wiring.  **Finds no core on a fresh
   un-provisioned SoM** (SES holds the M55) — provision first, see
   [`aen-provisioning.md`](aen-provisioning.md).  pyocd works too but
   its `-t` id depends on the installed `alif_ensemble-cmsis-dfp` pack.

2. **UART console capture.**  Wire USB-UART to UART0
   (`USB_UART_TXD`/`_RXD`), 115200 8N1, open a terminal and
   **log to a file** -- the boot ROM banner is the first thing
   you want captured.  Power-cycle; expect the
   `AlifSemi BootROM v1.x.y` banner within ~200 ms (§4).

3. **EEPROM / board_id read over BRD_I2C.**  Confirm the 24C128
   ACKs at `0x50` and dump the 128-byte Alp manifest (§3):

   ```bash
   i2cdetect -y 1                 # 0x50 must ACK
   i2cdump  -y 1 0x50 b           # full manifest hexdump
   ```

   Cross-check the magic/SKU/CRC fields against
   [`include/alp/hw_info.h`](../include/alp/hw_info.h).  An
   unprogrammed manifest is fine on a fresh assembly -- program it
   in §7 -- but a *non-ACKing* EEPROM is a wiring/pull-up fault.

4. **CC3501E PING / GET_VERSION.**  Bring the on-module Wi-Fi/BLE
   coprocessor to life over the inter-chip SPI1 bus.  Issue the
   two META-group opcodes from the bridge host driver (see the
   wire frame in `firmware/cc3501e/DESIGN.md`):
   `PING` (opcode `0x00`) then `GET_VERSION` (opcode `0x01`).
   A standalone host-side helper for the M55 side is **TBD**
   (only the device firmware + `flash.py` ship today), so drive
   it from app code via the bridge dispatch for now.

   * `PING` must return `RESP_OK` with empty data -- the liveness
     signal.
   * `GET_VERSION` must return the firmware's wire-protocol
     version; cross-check it against
     `firmware/cc3501e/prebuilt/CHANGELOG.md`.

   No `RESP_OK` usually means the CC3501E hasn't been flashed yet
   (`helper_firmware[].firmware_path` is still TBD in the SKU
   preset) or the SPI1 CS/IRQ wiring is off.

5. **Flash a Zephyr smoke image.**  Take the module over from its
   shipped self-test with the SDK's first-build example (§4).  To prove
   the toolchain + silicon first, build the upstream DevKit fallback
   (`ensemble_e8_dk/ae822fa0e5597ls0/rtss_he`); then switch to the
   carrier board for the carrier-accurate routing:

   ```bash
   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/peripheral-io/gpio-button-led
   west flash
   ```

   Expect the
   `[gpio] init button=BOARD_PIN_ENCODER_SW, led=BOARD_PIN_LED_RED`
   banner on the console.  This proves the toolchain, the board
   file, and `west flash` end-to-end before anything harder.

6. **Validate the MHUv2 doorbell round-trip with the real driver.**
   The mailbox controller is **no longer TBD**.  The inter-core IP
   is the **ARM MHUv2** (confirmed via the alifsemi/zephyr_alif
   fork DTS, corroborated by the Alif Linux BSP: `meta-alif-ensemble`
   + `linux_alif` wire `apss-mhu` -> `mhuv2.cfg`).  alp-sdk now ships
   a Zephyr MBOX-class driver for it under the **distinct** compatible
   `alif,mhuv2-mbox`
   ([`zephyr/drivers/mbox/mbox_alif_mhuv2.c`](../zephyr/drivers/mbox/mbox_alif_mhuv2.c),
   binding
   [`zephyr/dts/bindings/mbox/alif,mhuv2-mbox.yaml`](../zephyr/dts/bindings/mbox/alif,mhuv2-mbox.yaml)).
   The lead-part preset
   ([`.../E1M-AEN801.yaml`](../metadata/e1m_modules/E1M-AEN801.yaml))
   now sets `mailbox.controller: alif_mhuv2`, and the AEN801 M55-HE/HP
   overlays wire ipc0's `mboxes` to the APSS<->RTSS-HE MHU0 sender/
   receiver pair.

   > **vendor-ext, BENCH-UNVERIFIED.**  The driver, the MHUv2 register
   > map, and the node addresses/IRQs come from the ARM MHUv2 spec
   > (DDI 0515) + the fork DTS and have **not** yet been run on real
   > silicon.  The RTSS-HP MHU base in particular is unverified (the
   > fork's HP dtsi reused the HE addresses) -- see the "bench-confirm
   > HP MHU base" note in the HP overlay.  AEN801 is the lead part;
   > the rollout of this wiring to AEN301..701 follows after AEN801
   > bench-passes.

   On bench day, validate the doorbell round-trip with the multicore
   example (the step is now "make the real driver work", not "confirm
   the name"):

   This round-trip spans **HE↔HP**, so build both core images: the
   HE side here, and the peer HP image against
   `alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp`.

   ```bash
   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/multicore/mproc-mailbox
   # peer image: -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp
   west flash
   ```

   The HE↔HP round-trip on MBOX channel 0 must echo a 32-byte
   message.  If it fails, cross-check the MHUv2 frame base addresses
   and IRQ numbers in the overlay against the **Alif hand-written
   HW-config doc** and the generated board DTS before assuming a
   driver bug -- those values are the unverified part.

7. **Ethos-U sanity.**  Confirm the NPU is visible to the runtime
   before loading any model.  On the M55 side the Ethos-U55 is
   driven in-process by the TFLM + Ethos-U driver, so the sanity
   check is a built example that opens the NPU dispatcher and
   reports the detected variant:

   ```bash
   west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/aen/edgeai-vision-aen
   west flash
   ```

   Expect the console to report the Ethos-U variant the SKU
   preset declares (`u55` on AEN701; `u85` primary + dual `u55`
   on AEN801 -- see the `inference.npu_population` block).  A
   variant mismatch means the board DTS NPU node disagrees with
   the SoC JSON `npus[]`.

8. **Load a host-pre-compiled `.alpmodel` + Vela walkthrough.**
   Final gate: prove the end-to-end model path.

   1. On the **host**, compile + package the model(s) declared in
      `board.yaml` `models:` into `.alpmodel` packages (the
      backends, including Ethos-U / Vela, are derived from
      `som.sku`):

      ```bash
      alp model build --board board.yaml   # emits build/models/<name>.alpmodel
      ```

   2. Bundle the `.alpmodel` with the app image, flash, and run
      the inference example.  The runtime loads the package,
      picks the `vela_tflite` blob matching the on-die Ethos-U
      `accel_config` (`ethos-u85-256` / `ethos-u55-256` on
      AEN801), and runs the first inference on the NPU.

   3. Confirm the prediction matches the reference output within
      floating-point tolerance -- the same end-to-end gate the
      i.MX 93 bring-up uses in
      [`bring-up-imx93.md`](bring-up-imx93.md) §6.3, here driven
      from the M55 side rather than from Linux.

   If the `.alpmodel` has no Ethos-U blob the loader falls back to
   the CPU path -- correct behaviour, but it means the Vela target
   in your model-compile config didn't match this SoM's NPU.

Once §6's steps 0..8 all pass, the SoM is bench-validated; move
to §7 to write the production manifest.

## 7. Going to production

Once §6's runbook passes:

1. Use [`scripts/program_eeprom.py`](../scripts/program_eeprom.py)
   to write the production manifest (real serial, real mfg
   date, real SKU).  See
   [`docs/board-id.md`](board-id.md) for the BOARD_ID ADC
   companion path.
2. Sign the application image with MCUboot via
   [`zephyr/sysbuild/aen/sysbuild.conf`](../zephyr/sysbuild/aen/sysbuild.conf).
   Dev key under [`keys/`](../keys/); production key never
   leaves OPTIGA Trust M secure NVM.
3. Flash the signed image with `west flash`; the MCUboot
   secondary slot stays empty until OTA lands.

## 8. Troubleshooting

* **Boot ROM banner but then silence** -- usually a signed-
  image-rejected scenario.  Re-flash with the dev key or check
  the MCUboot trailer.
* **`i2cdetect` returns no slaves at all** -- BRD_I2C pull-ups
  missing or wrong voltage.  Standard Alp boards pull to
  1.8 V; some custom boards use 3.3 V (re-strap the SoC
  side accordingly).
* **PHY won't link** -- DP83825I requires its 25 MHz REFCLK
  before the strap latches.  Check `OSC_25M` on the board
  with a scope; the PHY won't link if the clock is missing at
  PHY-reset-release.

More patterns in [`docs/troubleshooting.md`](troubleshooting.md);
ask on [community.alplab.ai](https://community.alplab.ai/) for
anything not covered here.

## See also

- [`docs/soms/aen.md`](soms/aen.md) -- AEN one-pager.
- [`docs/getting-started.md`](getting-started.md) -- first-build
  walkthrough (the `gpio-button-led` example is what this
  bring-up flashes in §4).
- Internal AEN feature audit (private repo) -- which AEN silicon
  blocks the SDK does + doesn't expose; reference for "is X
  on the AEN supported" questions.
