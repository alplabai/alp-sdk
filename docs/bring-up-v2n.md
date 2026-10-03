# Bench bring-up — E1M-X V2N

Step-by-step procedure for bringing a freshly-assembled E1M-X V2N
module up on the bench.  Assumes you have an `E1M-X-EVK` board
(or a pin-compatible custom board), a SWD debug probe (J-Link or
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
* Board populated: at minimum, E1M-edge passthroughs + the 5 V
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
* Probe `VDD_3V3` and `VDD_1V8` on the board; should be at their
  ACT88760-stamped values.
* `DA9292.TW_N` line should be high (no thermal-warning); if low,
  the secondary PMIC is over temperature -- reduce load and retry.

## 2. SWD attach + GD32 firmware flash

The GD32 bridge firmware is **separate** from the Renesas-side
firmware -- the GD32 **chip** ships blank from GigaDevice; that's the
vendor's silicon state, not what a customer receives.  Alp Lab's own
production flash step is what turns that blank chip into a
**customer-shipped module** pre-flashed with the bridge firmware (see
[`docs/cross-platform-setup.md`](cross-platform-setup.md)
§2.3/§3.4/§4.3).  The "unprogrammed module" scenarios below are a
freshly-assembled board straight off SMT, before that production flash
step -- not the state a customer's module arrives in.  Two paths cover
the lifecycle:

### 2a. External probe (first power-on)

For an unprogrammed module on the bench the external probe is the
fastest route to first firmware:

1. Attach SWD probe to the V2N programming header (pads
   `GD32_SWDIO` = GD32 `PA13`, `GD32_SWCLK` = GD32 `PA14`).
2. Build the bridge firmware (needs the Arm GNU Toolchain
   (`arm-none-eabi-gcc`) -- this custom-carrier bring-up is exactly the
   optional trigger for that install; see
   [`docs/cross-platform-setup.md`](cross-platform-setup.md) §2.3/§3.4/§4.3):

   ```bash
   cd firmware/gd32-bridge
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
[`docs/test-plan.md`](test-plan.md)); the pin assignments above are
resolved (maintainer-confirmed 2026-05-12), not pending a schematic
revision -- see `metadata/chips/gd32_swd.yaml`.

### 2c. In-system upgrade over the bridge

Once a working bridge firmware is on the GD32, subsequent upgrades
flow through the application-bootloader OTA opcodes
(`CMD_OTA_*` in the reserved `0xF0..0xFF` range; see
[`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md) §10).
The full host-driver helper set ships on
`<alp/chips/gd32g553.h>`: `gd32g553_ota_begin()` /
`_write_chunk()` / `_verify()` / `_commit()` / `_get_state()` /
`_abort()`, gated at build time by `-DBRIDGE_OTA_PARTITIONED`
on the firmware side. The firmware path is silicon-validated
end-to-end (see [`docs/gd32-bridge.md`](gd32-bridge.md)).

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

* Read `GET_VERSION` -- the reply is the negotiated wire-protocol
  triple. Expect the MAJOR/MINOR your host gates on; the highest
  version on record as bench-validated end-to-end is **0.6** (see
  [`docs/gd32-bridge.md`](gd32-bridge.md)), and the wire history
  reaches 0.9. The host
  refuses a MAJOR mismatch, so a healthy link answers with the
  MAJOR/MINOR that `<alp/chips/gd32g553.h>`'s
  `GD32G553_HOST_PROTOCOL_MAJOR` expects (version history:
  [`docs/gd32-bridge-protocol.md`](gd32-bridge-protocol.md)).

## 4. Read the SoM hardware-info manifest

If the production-test programmer (`scripts/program_eeprom.py`) has
been run against this module, the on-module 24C128 EEPROM at
`ALP_E1M_I2C0` carries a 128-byte manifest with the SKU + hw_rev +
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

* **RV-3028-C7** (RTC): `/dev/rtc0` on Linux (`hwclock -r`), not the CM33
  -- CA55/Linux is now the sole master of RIIC8/BRD_I2C end to end; see
  [`docs/soms/v2n.md`](soms/v2n.md#real-time-clock). Set wall-clock, read
  back, confirm tick.
* **OPTIGA Trust M**: issue an I2C connectivity-probe (full APDU
  command set is v0.3.x follow-up).
* **TMP112**: read the temperature; should be within
  ±5 °C of ambient.
* **24C128 EEPROM**: read first 4 bytes; should be the manifest
  header magic, wire bytes `0x48 0x50 0x4C 0x41` (`HPLA` in a
  hexdump; decodes as the little-endian uint32 `0x414C5048`,
  `ALPH`).
* **DA9292 status**: `da9292_get_status()` -- expect CH1 PG=1,
  CH2 PG=0 (CH2 is the V2N-M1-only DEEPX rail), no fault bits.
* **ACT88760 status**: `act8760_get_status()` -- expect no
  `thermal_warning`, no `vsys_warning`, `vin_pok_ov=false`.

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
  default `-1`.  Wire the right bus id (ALP_E1M_I2C0 on V2N) and the
  EEPROM address (`0x50` strap default) in `prj.conf`.

## 7a. U-Boot environment and microSD card-detect (bench checks)

Design in [`soms/v2n.md`](soms/v2n.md#uboot-environment). Not yet run on
silicon; do them in order on a unit flashed with the new FIP and image.

1. **Boot partition size.** From Linux: `cat /sys/block/mmcblk0boot1/size`
   (512-byte sectors). Expect at least `0x1200` (the environment ends at
   byte `0x240000`). If smaller, stop: the offsets do not fit this eMMC.
2. **Which partition the ROM boots.** `mmc extcsd read /dev/mmcblk0 |
   grep PARTITION_CONFIG` -> `0x08` (boot partition 1 = `mmcblk0boot0`).
   Then compare `md5sum` of the first FIP-sized span of `mmcblk0boot0`
   and `mmcblk0boot1` against the bundle's FIP: record which one holds
   the running bootloader (open question: `write_emmc_boot` writes
   `mmcblk0boot1`).
3. **First boot.** Serial console on the first boot of the new FIP: expect
   one `bad CRC, using default environment`, then
   `ALP: environment initialised` and `Saving Environment to
   MMC... Writing to redundant MMC(0)... OK`.
4. **Second boot is silent.** Reboot: no `bad CRC` line, no `initialised`
   line, `Loading Environment from MMC... OK`.
5. **An allowlisted variable survives a reboot.** At the U-Boot prompt:
   `setenv bootcount 3; saveenv`, `reset`, then `printenv bootcount` ->
   `bootcount=3`. A variable not on the allowlist must NOT come back:
   `setenv alp_test 1; saveenv`, `reset`, `printenv alp_test` -> not defined.
6. **Redundancy.** After `saveenv`, corrupt copy 2 from Linux
   (`echo 0 > /sys/block/mmcblk0boot1/force_ro`, `dd if=/dev/zero
   of=/dev/mmcblk0boot1 bs=1 seek=$((0x230000)) count=16`,
   `echo 1 > /sys/block/mmcblk0boot1/force_ro`), reboot: U-Boot still
   loads `bootcount`; the next `saveenv` repairs the copy.
7. **Linux -> U-Boot.** In Linux: `fw_printenv bootcount` -> `3`;
   `fw_setenv bootcount 4`; reboot; U-Boot `printenv bootcount` -> `4`. If
   `fw_setenv` fails with a read-only error, `mmcblk0boot1`'s `force_ro`
   is set and the tool did not clear it; record it (the image then needs a
   udev rule or the OTA client must clear it).
8. **Boot control is the firmware's.** `fw_setenv bootcmd 'echo old'`,
   `fw_setenv bootdelay 5`, reboot: the unit still boots Linux and
   `printenv bootcmd bootdelay` shows the binary's values (on a production
   build the console stays locked). Clean up with `fw_setenv bootcmd` and
   `fw_setenv bootdelay` (unset).
9. **Provisioning does not clobber it.** Run the provisioning
   `write_emmc_boot` step on this unit, reboot, and confirm
   `printenv bootcount` is still set.
10. **Empty SD slot.** No card inserted, power-cycle, serial console: no
    `Card did not respond to voltage select! : -110` and no `mmc1`
    output; Linux boots from the eMMC.
11. **Card inserted.** Card with `boot/Image` and the dtb on partition 2:
    boots from the card (`root=/dev/mmcblk1p2`). Data-only card (no
    `boot/Image`): boots from the eMMC. Remove the card, power-cycle:
    step 10 again.
12. **Card-detect level.** At the U-Boot prompt with a card inserted:
    `alp_sd_present; echo $?` -> `0`; with the slot empty -> `1`.

## 8. Next steps

After the basic bring-up clears:

* Run the [test plan](test-plan.md) for the V2N family.
* If the board is V2N-M1, continue with
  [`bring-up-v2n-m1.md`](bring-up-v2n-m1.md) to bring the DEEPX
  rails up and load the NPU runtime.
