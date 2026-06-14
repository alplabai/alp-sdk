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

* Module populated:
  - **Alif Ensemble E3..E8** SoC (Cortex-M55 HP + HE, optional
    Cortex-A32 on E7/E8, Ethos-U55 NPU).
  - **TCAL9538** GPIO expander on BRD_I2C.
  - **24C128** EEPROM with the 128-byte Alp manifest at offset
    `0x0000`.
  - **CC3501E** Wi-Fi 6 + BLE coprocessor (TI SimpleLink).
  - **DP83825I** 10/100 Ethernet PHY (a single MAC; ET1 is
    E1M-X-only).
  - **CAM_MUX_PI3WVR626** camera multiplexer for the CSI side.
  - Optional sensor stuffing per the SKU (LSM6DSO IMU, BMP581
    barometer, TMP112 thermometer, INA236 power monitor).
* Board populated: at minimum, E1M-edge passthroughs + the
  5 V power input + JTAG/SWD header + USB-UART for console.

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
2. With the probe plugged in, run:

   ```bash
   pyocd cmd -t alif_e7
   ```

   Or the J-Link Commander equivalent.  Expect to read the
   Cortex-M55 HP core's `DPIDR` ≈ `0x6BA02477`.  Different value
   means either wrong target or SWD wiring is reversed.

3. Halt the core and read `CPUID` (M55 returns `0x410FD220` on
   r0p0).  Confirms you're talking to the right silicon.

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

4. The default firmware on a freshly-assembled module is the
   Alif demo image -- the BootROM hands off to it and you'll
   see Alif's demo banner.  To take it over with the Alp SDK,
   flash a built image:

   ```bash
   west alp-build -b alif_e7_dk_rtss_he examples/peripheral-io/gpio-button-led
   west flash
   ```

   Expected output on the UART: the
   `[gpio] init button=EVK_PIN_ENCODER_SW, led=EVK_PIN_LED_RED` banner
   from the SDK's first-build tutorial.

## 5. Peripheral sanity checks

Run these in order; each one exercises a different on-module
or on-board subsystem.

### 5.1 BRD_I2C: probe every on-module slave

The AEN module routes one shared BRD_I2C bus.  Drive an
i2cdetect from a built `i2c-scanner` example or via the
console:

| Slave | 7-bit addr | What |
|-------|------------|------|
| TCAL9538 | `0x72` | GPIO expander |
| 24C128 | `0x50` | EEPROM (manifest) |
| TMP112 | `0x48` | Thermometer (optional) |
| BMP581 | `0x47` | Barometer (optional) |
| LSM6DSO | `0x6A` | IMU (optional) |
| INA236 | `0x40` | Power monitor (optional) |
| OPTIGA TM | `0x30` | Secure element |

A missing slave that's *expected* per the SKU's
`metadata/e1m_modules/E1M-AEN<NNN>.yaml` `i2c_devices` block
is a real fault.  A missing optional slave is fine -- the SKU
preset declares which are populated.

### 5.2 OPTIGA Trust M sanity

```bash
west alp-build -b alif_e7_dk_rtss_he examples/optiga-trust-m-quickread
west flash
```

Expected on the console: `[optiga] product_info = ...`
followed by a 32-byte device-unique ID.  All-zeros or all-FFs
means the OPTIGA wasn't bonded out correctly on this assembly.

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

These are optional per the SKU and the board.  See
[`docs/soms/aen.md`](soms/aen.md) for the camera-mux truth table
and the per-SKU display options.

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
> bench bring-up centres on the **E8** part.  The generated AEN801
> M55 board target is `alp_e1m_aen801_m55_hp` (HP core) /
> `alp_e1m_aen801_m55_he` (HE core) per the SKU preset's `topology`.
> **No published board file backs it yet, and the Alif public Zephyr
> SDK does not ship an E8 board either** — `alifsemi/zephyr_alif`
> `boards/alif/` has `alif_e7_dk` / `alif_e7_ak` / `alif_b1_dk` /
> `alif_e1c_dk`, but no E8 — so the E8 M55 board must be generated by
> alp-sdk (or ported). The exact `west build`/`west flash` target is
> **TBD until it lands** (see
> [`docs/porting-new-som.md`](porting-new-som.md)).  The commands below
> show the Alif E7 DevKit naming (`alif_e7_dk_..._rtss_he`) purely as the
> invocation *template* — substitute the AEN801 target once it exists.

0. **Current-limited power-on + rail check.**  Bench supply at a
   **1 A** limit on V_IN (§1).  Power on, watch steady-state
   current settle to **~80..150 mA**, then probe V_3V3
   (3.30 V ±2 %) and V_CORE (~0.90 V ±3 %).  A current trip or a
   missing rail stops here -- see §1's PMIC-sequencer note.

1. **SWD/J-Link attach + CPUID read.**  Wire the probe (§2),
   then:

   ```bash
   pyocd cmd -t alif_e7        # or the J-Link Commander equivalent
   ```

   Read `DPIDR` ≈ `0x6BA02477`, halt, and confirm the M55 HP
   `CPUID` = `0x410FD220` (r0p0).  Wrong values = wrong target
   or reversed SWD wiring.

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

5. **Flash a Zephyr smoke image.**  Take the module over from the
   Alif demo image with the SDK's first-build example (§4):

   ```bash
   west alp-build -b alif_e7_dk_rtss_he examples/peripheral-io/gpio-button-led
   west flash
   ```

   Expect the
   `[gpio] init button=EVK_PIN_ENCODER_SW, led=EVK_PIN_LED_RED`
   banner on the console.  This proves the toolchain, the board
   file, and `west flash` end-to-end before anything harder.

6. **Confirm the `mailbox.controller` value against the Alif HW
   config.**  The SKU preset
   ([`metadata/e1m_modules/E1M-AEN701.yaml`](../metadata/e1m_modules/E1M-AEN701.yaml),
   `.../E1M-AEN801.yaml`) ships `mailbox.controller: TBD` with a
   **candidate** of `alif_mhu` (Arm MHU; MHU0 + MHU1, 32-byte
   payload limit -- see
   [`docs/tutorials/15-mproc-mailbox.md`](tutorials/15-mproc-mailbox.md)).
   The Alif Linux BSP corroborates **MHUv2** (`meta-alif-ensemble` +
   `linux_alif` enable `apss-mhu` -> `mhuv2.cfg`), so the inter-core
   mailbox IP is MHUv2 -- only the exact Zephyr binding name is unconfirmed.
   On bench day, confirm the actual Zephyr binding name against
   the **Alif hand-written HW-config doc** and the generated
   board DTS, then promote TBD to the confirmed value in **both**
   AEN701 + AEN801 presets.  Smoke-test it with the multicore
   example:

   ```bash
   west alp-build -b alif_e7_dk_rtss_he examples/multicore/mproc-mailbox
   west flash
   ```

   The HE↔HP round-trip on MBOX channel 0 must echo a 32-byte
   message.  Until this step passes, treat `mailbox.controller`
   as unverified -- do **not** commit a guessed value.

7. **Ethos-U sanity.**  Confirm the NPU is visible to the runtime
   before loading any model.  On the M55 side the Ethos-U55 is
   driven in-process by the TFLM + Ethos-U driver, so the sanity
   check is a built example that opens the NPU dispatcher and
   reports the detected variant:

   ```bash
   west alp-build -b alif_e7_dk_rtss_he examples/aen/edgeai-vision-aen
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
