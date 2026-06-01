# RZ/V2N Cortex-M33 secure-boot deploy (E1M-V2M101 / V2N-M1)

How the on-module **Cortex-M33 system-manager** firmware (Zephyr, e.g.
`examples/v2n/v2n-gd32-bridge-ping`) is started by the **secure boot chain** —
not by a U-Boot/`/dev/mem` poke (that path fights TZC and is wrong). Verified on
silicon 2026-06-01: the M33 firmware is loaded + started by BL2 and the
A55/Linux boots cleanly on the same BL2.

## Mechanism (already in the Renesas RZ/V2N TF-A)
The v6.30 TF-A has a built-in CM33 boot path, gated by `PLAT_M33_BOOT_SUPPORT`:

- `bl2_plat_mem_params_desc.c`: image `BL22_IMAGE_ID` (the M33 FW) loads to
  `BL22_BASE = 0x08000000` (SRAM0), max `0x60000`.
- `plat_storage.c`: BL22 source = **xSPI offset `0x200000`** (`RZV2N_M33_FW_OFFSET`,
  192 KB slot), separate from the FIP (which stays at `0x60000`).
- `bl2_plat_setup.c` (`bl2_el3_plat_prepare_exit`): if the CM33 isn't already
  booted, `sys_m33_core_boot_op()` sets `SYS_MCPU_CFG2 = BL22_S_VECTOR = 0x08003000`
  (secure) / `SYS_MCPU_CFG3 = 0x18003000` (non-secure), then `cpg_cm33_setup()`
  releases the CM33 (`CPG_RST_1 @ 0x10420904`, RSTB3/4/5).

So the M33 FW is a raw image at xSPI `0x200000`, loaded to `0x08000000`, and the
CM33 boots at `0x08003000`.

## The two-line TF-A enablement — `docs/rzv2n-tfa-m33-boot.patch`
Apply against the v6.30 TFA tree, then rebuild BL2 (`build_custom_bl2_v630.sh`).

1. `v2n_common.mk`: `PLAT_M33_BOOT_SUPPORT := 1` (keep `BOOT_TFA_USING_CM33 := 0`
   so the CA55-boot flash layout is unchanged: BL2@0, FIP@0x60000, M33 FW@0x200000).
2. `plat_storage.c`: Renesas coupled the M33-boot path to `PLAT_SYSTEM_SUSPEND`,
   so it doesn't compile standalone. Fix makes it self-contained:
   include `plat_tbbr_img_def.h` (defines `BL22_IMAGE_ID`) under
   `(PLAT_SYSTEM_SUSPEND || PLAT_M33_BOOT_SUPPORT)`, and read the M33 FW via
   **`memmap`** (`memdrv_dev_handle` + `open_memmap`) like the FIP — the xSPI
   memory-mapped window (`0x20000000` + 256 MB) covers `0x20200000`, and the
   suspend-only `xspidrv` device stays out of the cold-boot path.

## The M33 firmware image
`zephyr.bin` is linked at `0x08003000` (board `alp_e1m_v2m101_m33_sm`,
`sram: memory@8003000`). BL2 loads the raw image at `0x08000000`, so the image is
**`0x3000` zero-padding + `zephyr.bin`** (Zephyr's vector lands exactly at
`0x08003000 = BL22_S_VECTOR`). ~67 KB, well under the `0x30000` slot.

```sh
head -c 12288 /dev/zero > pad.bin && cat pad.bin zephyr.bin > m33_fw.bin
```

## Flash flow (from running Linux, no Flash Writer needed)
Board mtd: `mtd0="bl2"`@0x0, `mtd1="fip"`@0x60000. The M33 slot `0x200000` =
**mtd1 offset `0x1a0000`** (past the ~1.1 MB FIP).

```sh
# (transfer m33_fw.bin + the new bl2_bp_spi.bin to the board, e.g. via socat)
mtd_debug read /dev/mtd0 0 0x60000 /root/mtd0_backup.bin   # back up current BL2
flash_erase /dev/mtd1 0x1a0000 17 && mtd_debug write /dev/mtd1 0x1a0000 67224 m33_fw.bin
flashcp -v bl2_bp_spi.bin /dev/mtd0
reboot
```
Recovery if a bad BL2 won't boot: SCIF Flash Writer + a known-good `bl2_bp_spi*.srec`.

## Verify from the GD32 side (J-Link, no CM33 console needed)
The CM33's Zephyr console is `sci0`/P05 (a separate UART). Instead, read the GD32
bridge over SWD (live reads, no halt): `JLink -device GD32G553MEY7TR`,
`mem8 0x20000000, 0x10`. A serviced PING leaves `spi_rx_buf = A5 00 FF 84`
(symbols from `firmware/gd32-bridge/build/gd32/gd32-bridge`:
`spi_rx_buf@0x20000000`, `spi_tx_buf@0x2000004c`, `spi_tx_cursor@0x20000098`).

## ⚠ Open: the M33→GD32 SPI link is SCI7 Simple-SPI, not the dedicated SPI_B
On-silicon J-Link verification showed the GD32 receiving **zero SPI bytes**. Root
cause: the board wires the GD32 to **P76(MOSI)/P77(MISO)/P96(SCLK)/P97(CS)**, which
per the RZ/V2N PFC (Table 1.2-3) are **`MOSI7/MISO7/SCK7/SS7` = SCI channel 7**
(`sci7@42802800`) in clock-sync/Simple-SPI mode, at functions **P76=1, P77=1,
P96=2, P97=2** (the bring-up's `func5` was inferred and is wrong → routes to
CTXDP3/ADC). So the `spi_renesas_rz_spi_b.c` (FSP `r_spi_b`, dedicated SPI) driver
is the wrong peripheral, and the RZ FSP ships no SCI-SPI module (only RA has
`r_sci_b_spi`). **Pending fix:** port RA `r_sci_b_spi` → RZ + a Zephyr
`renesas,rz-sci-b-spi` driver/binding + a DT SCI7 SPI child + the corrected pinmux.
