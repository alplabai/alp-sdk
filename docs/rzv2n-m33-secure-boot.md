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

## The two-line TF-A enablement — `meta-alp-sdk/recipes-bsp/trusted-firmware-a/trusted-firmware-a/0001-rzv2n-boot-the-CM33-from-xSPI.patch`
Applied by `trusted-firmware-a_%.bbappend` on every `rzv2n-family` MACHINE, so
`bitbake firmware-pack` produces a BL2 that boots the CM33 (#2354).

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

<!-- cross-platform-lint:ignore -->
These commands run in a shell **on the board itself** (over the serial
console after the transfer step), not on the developer host -- the
target always runs embedded Linux regardless of the host OS you're
developing from, so `/root/...` and the MTD utilities below are correct
as written on every host.
```sh
# (transfer m33_fw.bin + the new bl2_bp_spi.bin to the board, e.g. via socat)
mtd_debug read /dev/mtd0 0 0x60000 /root/mtd0_backup.bin   # back up current BL2
flash_erase /dev/mtd1 0x1a0000 17 && mtd_debug write /dev/mtd1 0x1a0000 67224 m33_fw.bin
flashcp -v bl2_bp_spi.bin /dev/mtd0
reboot
```
<!-- cross-platform-lint:resume -->

> **Now automated for the M33 image.** The M33 firmware write above
> (`flash_erase` + `mtd_debug write` to `/dev/mtd1 0x1a0000`) is exactly what
> the `rzv2n_mtd_flash` west runner performs end-to-end, with a byte-exact md5
> readback: `west flash --host <board-ip>` for the `alp_e1m_v2n101_m33_sm` /
> `alp_e1m_v2m101_m33_sm` boards. The manual steps here remain the recovery
> path and the only way to also reflash BL2 (`bl2_bp_spi.bin`).
Recovery if a bad BL2 won't boot: SCIF Flash Writer + a known-good `bl2_bp_spi*.srec`.

## Lifecycle: TF-A boots the CM33, Linux remoteproc attaches (opt-in, bench-pending)

Decision (Q52): adopt Renesas' RZ/V2N Linux remoteproc driver and **keep TF-A
boot**.  BL2 still starts the CM33 at power-on from the xSPI image (above);
Linux then *attaches* to the running core and can stop, start and reload it,
so a CM33 update no longer needs a SoC reboot.

| Phase | Who | What |
|-------|-----|------|
| power-on | TF-A BL2 | loads the xSPI M33 slot to `0x08000000`, sets `SYS_MCPU_CFG2/3`, releases the CM33 reset |
| kernel probe | `rz_rproc` | sees the CM33 out of reset (`CPG_RSTMON_0`), rproc state `detached` |
| attach | `echo start > state` | no-op attach (`rz_rproc_attach`), state `attached`, CM33 untouched |
| stop | `echo stop > state` | CM33 clock off, reset asserted, the carveouts (CM33 SRAM `0x08000000`, 1 MiB, and the OpenAMP window `0x4f700000`, 9 MiB) zeroed |
| reload | `echo <elf> > firmware; echo start > state` | ELF segments written to SRAM, `SYS_MCPU_CFG2/3` set, CM33 released |

Enable it with `ALP_V2N_REMOTEPROC = "1"` in `local.conf` (default `"0"`
until the bench proves it).  That applies
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0018`-`0021`, merges
`remoteproc.cfg` and installs the `cm33_rproc` node
(`e1m-v2n-remoteproc.dtsi`).  The driver is the Renesas RZ Multi-OS Package
v4.2.0's (`rz_rproc.c`, GPL-2.0, patches `0019`/`0020` imported unmodified
with Renesas authorship); the Alp changes are `0018` (CPG is a syscon) and
`0021` (`alp,rz-userspace-ipc`).

Commands on the board:

```sh
RP=/sys/class/remoteproc/remoteproc0
cat $RP/name $RP/state            # remoteproc-cm33, detached
echo start > $RP/state            # attach            -> attached
echo stop  > $RP/state            # stop the CM33     -> offline
echo m33_sm.elf > $RP/firmware    # /lib/firmware/m33_sm.elf (zephyr.elf)
echo start > $RP/state            # reload + boot     -> running
```

Rules that follow from the design:

- **The kernel does not own the rings.**  The userspace OpenAMP master
  (`yocto_uio_drv.c`, `/dev/uio*`) does; the driver's `kick` is a stub.
  `alp,rz-userspace-ipc` keeps the CM33 resource table away from the
  remoteproc core so no virtio rpmsg device appears over the same vrings.
  Close every `alp_rpc_*` client before `stop` (the window is zeroed) and
  open again afterwards: the CM33 republishes its beacon (`rsctbl+0xFF0`)
  and the attach epoch restarts at `0`.
- **Window.**  The OpenAMP window stays at A55 `0x4f700000` / CM33
  `0x9f700000` (9 MiB), inside `openamp_reserved`
  (`e1m-v2n-som.dtsi`).  Renesas' driver only treats A55 `0x40010000`
  to `0x43EFFFFF` as a CA55-space address, but that range sits in the first
  128 MB that `memory@48000000` leaves to the secure area (an NS read of
  `0x40000000`-`0x47FFFFFF` returned `0xFF`, see the board dts), and the
  check only gates CA55-space addresses of resource-table entries the kernel
  never parses here.  The DT therefore carries no `cm33_ddr` reg (Renesas
  zeroes every carveout on `stop`).  The window is hard-coded in
  `scripts/gen_zephyr_board.py` and the SoM dtsi, not derived from SoM
  metadata: a generator from `memory_map:` is follow-up.
- **TZC.**  Attach touches no memory.  `stop` and reload write CM33 SRAM from
  Linux, which needs the non-secure access Renesas' TF-A patch
  `0001-Change-MCPU-sram-to-unpriv.patch` grants
  (`ENABLE_SRAM_REGION_ACCESS_MCPU`: SRAM region 0 becomes NS-accessible,
  which loosens the secure SRAM lock-down).  Our TF-A bbappend does not carry
  it yet; **do not `stop` before it does and the security trade-off is
  signed off** (a blocked write is a bus error).
- `/lib/firmware/m33_sm.elf` is not installed by any recipe yet; copy
  `zephyr.elf` over by hand for the bench.

### Doorbell: SPI 404 default, SPI 385 selectable

The CM33 -> CA55 doorbell is MHU-B SWINT unit 12 (GIC_SPI 404, bench-proven
#697) by default.  Renesas documents `rsp_ch8_ns` (RSP of NS slot 8, GIC_SPI
385, level-high) instead; the A55 -> CM33 kick stays message channel 5.
Select 385 consistently on all three sides:

| Side | Switch |
|------|--------|
| kernel DT | `ALP_V2N_DOORBELL_SPI = "385"` (rewrites `e1m-v2n-doorbell.dtsi`) |
| libalp_sdk | the same variable adds `-DALP_SDK_V2N_DOORBELL_RSP_CH8=ON` (alp-sdk recipe) |
| CM33 | `CONFIG_ALP_V2N_DOORBELL_RSP_CH8=y` |

Register offsets: `include/alp/protocol/v2n_mhu_doorbell.h` (CM33 SET
`0x50480114`, A55 clear `0x10480118` for 385; `0x504808C4` / `0x104808C8` for
404).

### Bench verification plan (none of this is verified yet)

Run in this order; stop at the first failure.

1. Build with `ALP_V2N_REMOTEPROC = "1"`, flash, cold-cycle.  `dmesg | grep
   rz-rproc` shows `probed`; `cat $RP/state` is `detached`; the CM33 beacon
   (`devmem 0x4f700ff8 32`) still ticks (the probe must not disturb the core).
2. `echo start > $RP/state`: `attached`, beacon still ticking, no new
   `/dev/rpmsg*` and no virtio device (`ls /sys/bus/virtio/devices` empty).
3. Run `rpmsg_v2n_consumer` (`v2m103-rpmsg-echo-uio.yaml`): 4/4 echo on the
   default doorbell (404) with remoteproc attached.
4. Probe NS access to CM33 SRAM from Linux
   (`devmem 0x08003000 32` read) to learn whether TF-A needs the TZC change
   before step 5.  If it faults, stop here and land the TF-A change first;
   if it reads, `touch /etc/alp-hil-rproc-stop` to let the HIL spec run the
   stop/start half.
5. With the TZC change: close clients, `echo stop > $RP/state`: state
   `offline`, beacon frozen (`devmem 0x4f700ff8 32` read twice, equal; the
   window reads zero after the zeroing, so also magic `0`), GD32 link silent.
6. `echo start > $RP/state` with the ELF set: state `running`, beacon magic
   `0xA10D0683` and version back, heartbeat ticking, attach epoch `0`.
7. `rpmsg_v2n_consumer` again: 4/4 echo, no cold cycle in between.
8. Repeat 3 and 5-7 with `ALP_V2N_DOORBELL_SPI = "385"` (all three sides):
   `/proc/interrupts` shows `GICv3 417 Level mhu-uio` (INTID = SPI + 32) and
   its count rises with each reply.  Only after steps 1-8 pass on a cold
   boot and a warm cycle flip the defaults.

HIL spec: `tests/hil/v2m103-x-evk/v2m103-remoteproc-lifecycle.yaml`.

## Verify from the GD32 side (J-Link, no CM33 console needed)
The CM33's Zephyr console is `sci0`/P05 (a separate UART). Instead, read the GD32
bridge over SWD (live reads, no halt): `JLink -device GD32G553MEY7TR`,
`mem8 0x20000000, 0x10`. A serviced PING leaves `spi_rx_buf = A5 00 FF 84`
(symbols from `gd32-bridge-firmware:build/gd32/gd32-bridge`:
`spi_rx_buf@0x20000000`, `spi_tx_buf@0x2000004c`, `spi_tx_cursor@0x20000098`).

## Resolved: the M33→GD32 SPI link is SCI7 Simple-SPI, not the dedicated SPI_B
On-silicon J-Link verification showed the GD32 receiving **zero SPI bytes**. Root
cause: the board wires the GD32 to **P76(MOSI)/P77(MISO)/P96(SCLK)/P97(CS)**, which
per the RZ/V2N PFC (Table 1.2-3) are **`MOSI7/MISO7/SCK7/SS7` = SCI channel 7**
(`sci7@42802800`) in clock-sync/Simple-SPI mode, at functions **P76=1, P77=1,
P96=2, P97=2** (the bring-up's `func5` was inferred and is wrong → routes to
CTXDP3/ADC). So the `spi_renesas_rz_spi_b.c` (FSP `r_spi_b`, dedicated SPI) driver
is the wrong peripheral, and the RZ FSP ships no SCI-SPI module (only RA has
`r_sci_b_spi`). **Fix landed:** the RA `r_sci_b_spi` port is in-tree as
`zephyr/drivers/spi/spi_renesas_rz_sci_b.c` with the `renesas,rz-sci-b-spi`
binding (`zephyr/dts/bindings/spi/renesas,rz-sci-b-spi.yaml`), the DT SCI7 SPI
child, and the corrected pinmux; SCI7 Simple-SPI is silicon-validated and is
the permanent transport — see `docs/gd32-link-sci7-next-rev.md`.
