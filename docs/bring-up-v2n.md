# Bench bring-up — E1M-X V2N

Step-by-step procedure for bringing a freshly-assembled E1M-X V2N
module up on the bench.  Assumes you have an `E1M-X-EVK` carrier
(or a pin-compatible custom carrier), a SWD debug probe (J-Link or
ST-Link), an external 5 V bench supply, a USB-UART adapter, and a
1 Gb Ethernet link partner.

> **For V2N-M1** (with DEEPX populated), follow this guide first to
> a working RZ/V2N boot, then continue with
> [`bring-up-v2n-m1.md`](bring-up-v2n-m1.md) for the DEEPX rails +
> bring-up sequencer.

## 0. Pre-flight

Inventory check before powering anything:

* Module populated: ACT88760 (primary PMIC), DA9292 (secondary PMIC),
  GD32G553 (supervisor MCU), RV-3028-C7 (RTC), OPTIGA Trust M
  (secure element), TMP112 (temp sensor), N24S128 (EEPROM),
  Murata LBEE5HY2FY-922 (Wi-Fi/BT), 2x RTL8211FDI (Ethernet PHYs),
  5L35023B (audio clock generator), and the on-module eMMC + xSPI.
* Carrier populated: at minimum, E1M-edge passthroughs + the 5 V
  power input + JTAG/SWD header + USB-UART for console.

## 1. First-power smoke test

1. Connect a current-limited bench supply (1 A limit) to V_IN.
2. Power on; watch the supply.  Steady-state current should be
   ~250-400 mA with the SoC idle in U-Boot prompt.
3. Confirm `ACT88760_nRESET` releases (use a scope on test point
   `TPS-ACT-NRST` if instrumented; otherwise infer from the SoC's
   boot console).
4. UART console (E1M `UART0`) should print U-Boot banner within
   ~1.5 s of power-on.

If the SoC console stays silent:

* Probe `VDD_0V8` (DA9292 CH1) on a test point — should be 0.8 V ± 1 %.
* Probe `VDD_3V3` and `VDD_1V8` on the carrier; should be at their
  ACT88760-stamped values.
* `DA9292.TW_N` line should be high (no thermal-warning); if low,
  the secondary PMIC is over temperature -- reduce load and retry.

## 2. SWD attach + GD32 firmware flash

The GD32 bridge firmware is **separate** from the Renesas-side
firmware -- the GD32 ships blank from GigaDevice.  Two paths cover
the lifecycle:

### 2a. External probe (first power-on)

For an unprogrammed module on the bench the external probe is the
fastest route to first firmware:

1. Attach SWD probe to the V2N programming header (pads
   `GD32_SWDIO` = GD32 `PA13`, `GD32_SWCLK` = GD32 `PA14`).
2. Build the bridge firmware:

   ```bash
   cd alp-sdk/gd32-bridge
   cmake -B build -DCMAKE_TOOLCHAIN_FILE=toolchain/arm-none-eabi.cmake
   cmake --build build
   ```

3. Flash `build/gd32-bridge.elf` via OpenOCD / Segger J-Link.
4. Verify the bridge responds to `PING` from the host side -- either
   over SPI or I2C (see step 3 below).

### 2b. Host-driven SWD recovery (no external probe)

With `GD32_SWDIO` on Renesas `P70` and `GD32_SWCLK` on Renesas `P71`
(per the 2026-05-12 hardware decision), the Renesas host itself can
reflash the GD32 over three GPIOs.  This is the path the field-
update flow uses when the application bootloader is unreachable
(corrupt bridge image, factory first-flash, dev-board bring-up).

The driver lives at [`chips/gd32_swd/`](../chips/gd32_swd/) with the
header at [`<alp/chips/gd32_swd.h>`](../include/alp/chips/gd32_swd.h):

```c
gd32_swd_t swd;
gd32_swd_init(&swd, /*swdio*/ pin_swdio, /*swclk*/ pin_swclk, /*nrst*/ pin_nrst);
gd32_swd_connect(&swd);            /* line-reset + JTAG-to-SWD + IDCODE read */
gd32_swd_halt(&swd);               /* halt Cortex-M33 cleanly */
gd32_swd_flash_erase(&swd, GD32_SWD_FMC_FLASH_BASE, image_size);
gd32_swd_flash_write (&swd, GD32_SWD_FMC_FLASH_BASE, image_bytes, image_size);
gd32_swd_flash_verify(&swd, GD32_SWD_FMC_FLASH_BASE, image_bytes, image_size);
gd32_swd_reset_and_run(&swd);
```

Driver status is `partial` until exercised on real silicon (see
[`docs/test-plan.md`](test-plan.md)); pin assignments on V2N are
TBD pending the next schematic revision.

### 2c. In-system upgrade over the bridge

Once a working bridge firmware is on the GD32, subsequent upgrades
flow through the application-bootloader OTA opcodes
(`CMD_OTA_*` in the reserved `0xF0..0xFF` range; see
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10).
The host driver helpers for these opcodes are not yet on
`<alp/chips/gd32g553.h>` -- the firmware-side handlers reply
`STATUS_NOSUPPORT` until the bodies land.

## 3. Confirm the host ↔ GD32 bridge link

From a Zephyr app on the RZ/V2N (or a minimal U-Boot script
exercising the I2C bus):

* Open the `BRD_I2C` bus.  Issue an I2C transaction to the GD32's
  configured slave address (`0x70` by default):

  ```
  write [reg=0x00][CMD=PING=0x00][CRC_lo][CRC_hi]
  read  [STATUS][CRC_lo][CRC_hi]
  ```

  Expected: status byte `0x00`, CRC valid.

* Optional: confirm the SPI fast path.  Issue an SPI write + read
  pair per [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md)
  §4.

* Read `GET_VERSION` -- expect `0.1.0` (the v0.3 candidate firmware).

## 4. Read the SoM hardware-info manifest

If the production-test programmer (`tools/program_eeprom.py`) has
been run against this module, the on-module 24C128 EEPROM at
`E1M_I2C0` carries a 128-byte manifest with the SKU + hw_rev +
serial number.  Confirm:

```c
alp_hw_info_t info;
alp_hw_info_read(&info);
printf("SoM: family=%s sku=%s hw_rev=%s serial=%s\n",
       info.som_family, info.som_sku, info.som_hw_rev, info.som_serial);
```

Expect non-empty fields; the example
[`v2n-board-id-readout`](../examples/v2n/v2n-board-id-readout/) shells
this out as a standalone reference.  If the manifest is blank,
factory programming has not run; flag for production-test follow-up.

## 5. Bring up the two Ethernet PHYs (RTL8211FDI)

Each PHY is reachable over its own MDIO bus on the Renesas side
(MDC + MDIO routes documented in
[`metadata/e1m_modules/v2n/renesas-peripheral-map.tsv`](../metadata/e1m_modules/v2n/renesas-peripheral-map.tsv)).
Wrap the Renesas MDIO controller in a callback that the
`<alp/chips/rtl8211fdi.h>` driver consumes:

```c
static int my_mdio_read(uint8_t phy_addr, uint8_t reg, uint16_t *val, void *user) {
    return mdio_read(user, phy_addr, reg, val); /* zephyr mdio.h */
}
static int my_mdio_write(uint8_t phy_addr, uint8_t reg, uint16_t val, void *user) {
    return mdio_write(user, phy_addr, reg, val);
}

rtl8211fdi_t phy0;
rtl8211fdi_init(&phy0, /*phy_addr*/ 0, my_mdio_read, my_mdio_write, mdio_dev);
rtl8211fdi_soft_reset(&phy0, 500000);
rtl8211fdi_restart_autoneg(&phy0);

bool up; rtl8211fdi_speed_t speed; bool full_duplex;
rtl8211fdi_get_link(&phy0, &up, &speed, &full_duplex);
```

Expected: PHYID1 reads `0x001C` (Realtek OUI).  After ~3-5 s with a
1 Gb link partner, `get_link` returns `up=true`,
`speed=RTL8211FDI_SPEED_1000M`, `full_duplex=true`.

## 6. Sanity-check the rest of the on-module fleet

* **RV-3028-C7** (RTC): set wall-clock, read back, confirm tick.
* **OPTIGA Trust M**: issue an I2C connectivity-probe (full APDU
  command set is v0.3.x follow-up).
* **TMP112**: read the temperature; should be within
  ±5 °C of ambient.
* **24C128 EEPROM**: read first 8 bytes; should be the manifest
  header magic `ALPH` (`0x41 0x4C 0x50 0x48`).
* **DA9292 status**: `da9292_get_status()` -- expect CH1 PG=1,
  CH2 PG=0 (CH2 is the V2N-M1-only DEEPX rail), no fault bits.
* **ACT88760 status**: `act8760_get_status()` -- expect no
  `thermal_warning`, no `sys_warning`, `ilim_warning=false`.

## 7. Common gotchas

* **Module boots but Ethernet is dead.** Check the 1.8 V rail
  feeds both PHYs and the MDC/MDIO pull-ups (1 kΩ to `VDD_1V8`)
  are present.  R39/R40 (ET0) + R56/R57 (ET1) per
  [`renesas-peripheral-map.tsv`](../metadata/e1m_modules/v2n/renesas-peripheral-map.tsv).

* **Bridge `PING` succeeds but `GET_VERSION` returns bad CRC.**
  Most likely cause is a slow GD32 ISR -- the firmware needs to
  finish processing within the host's inter-transaction gap (see
  [`gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §4.1).
  The host driver returns `ALP_ERR_IO`, which is safe to retry.

* **`alp_hw_info_read` returns `ALP_ERR_IO`** (CRC mismatch).
  The EEPROM is reachable but the manifest at offset 0 is wrong.
  Inspect the raw 128 bytes with `eeprom_24c128_read` and compare
  against the format in `<alp/hw_info.h>`.

* **`alp_hw_info_read` returns `ALP_ERR_NOSUPPORT`.**
  Kconfig `CONFIG_ALP_SDK_HW_INFO_EEPROM_I2C_BUS_ID` is set to its
  default `-1`.  Wire the right bus id (E1M_I2C0 on V2N) and the
  EEPROM address (`0x50` strap default) in `prj.conf`.

## 8. Next steps

After the basic bring-up clears:

* Run the [test plan](test-plan.md) for the V2N family.
* If the carrier is V2N-M1, continue with
  [`bring-up-v2n-m1.md`](bring-up-v2n-m1.md) to bring the DEEPX
  rails up and load the NPU runtime.
