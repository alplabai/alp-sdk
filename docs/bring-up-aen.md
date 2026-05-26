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
  - **24C128** EEPROM with the 128-byte ALP manifest at offset
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

The 24C128 carries the 128-byte ALP manifest at offset
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
   see Alif's demo banner.  To take it over with the ALP SDK,
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

## 6. Going to production

Once §1..5 pass:

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

## 7. Troubleshooting

* **Boot ROM banner but then silence** -- usually a signed-
  image-rejected scenario.  Re-flash with the dev key or check
  the MCUboot trailer.
* **`i2cdetect` returns no slaves at all** -- BRD_I2C pull-ups
  missing or wrong voltage.  Standard ALP boards pull to
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
