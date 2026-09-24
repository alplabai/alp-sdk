### Added — U-Boot sequences the V2N-M1 DEEPX 0.75 V core rail before releasing `M1_RESET` (#2045)

The V2N-M1 SoM's DEEPX DX-M1 core rail (DA9292 CH2, 0.75 V) had no
sequencing step: `board_late_init()` released `M1_RESET` and enabled the
PCIe mux purely on the on-module EEPROM manifest match, with nothing
bringing CH2 up first. The DA9292-AROVx OTP variant this SoM uses boots
with `CH2_VSTEP=1` (doubled voltage range, `PMC_CTRL_01` reset `0x80`);
releasing `M1_RESET` before CH2 is confirmed at 0.75 V and power-good
risked an over-voltage event on the DEEPX core rail, not merely a
brownout.

`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`
adds `alp_deepx_rail_bringup()`, run in `board_late_init()` before
`alp_deepx_pcie_bringup()` (patch 0001). It programs CH2 to 0.75 V over
DM I2C bus 8 (RIIC8/BRD_I2C, DA9292 at `0x1E`), verifies every write by
read-back, is idempotent across a warm reboot (never clears a validly-up
`CH2_EN`), then enables the channel and releases P64
(`DEEPX_CORE_0P75_EN`) only after polling P65 (`DEEPX_PWR_EN_REQ`) and
confirming `CH2_PG` with no UV/OV/OC. `alp_deepx_pcie_bringup()` now runs
only when the rail step succeeds. Gated on the new
`CONFIG_ALP_E1M_DEEPX_RAIL` (default `n`, wired via `deepx-rail.cfg` for
the V2M MACHINEs only) OR the existing EEPROM-manifest check, so V2N base
images touch nothing on the DA9292.

Three hardening fixes on top of the first draft: the P65 read now uses
the dedicated PFC terminal-input register (`PFC_BASE+0x0820+n`) instead
of the P output-latch register (which returned the last value
*written*, not the live pin level); every program-phase abort forces P64
low / `M1_RESET` asserted via a shared `alp_deepx_rail_safe_off()`
helper; `CH2_VSTEP` is re-checked immediately before the `CH2_EN` write
and aborts if it is ever found set -- the exact over-voltage condition
this sequence exists to prevent; and the PG-timeout failure message now
names a possible EN2-gates-CH2 hardware-wiring question (see
`docs/bring-up-v2n-m1.md` §2).

**Maintainer decision carried through in full:** Cortex-A55/Linux is the
sole master of RIIC8/BRD_I2C; the CM33 must never master it or claim
P64/P65.

- `src/zephyr/v2n_power_mgmt.c` / `.h`, `CONFIG_ALP_SDK_V2N_POWER_MGMT`, and
  the V2N supervisor's BRD_I2C transport
  (`CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID`/`_I2C_ADDR`/`_I2C_BITRATE_HZ`,
  `alp_z_v2n_supervisor_brd_i2c_acquire`/`_release`) are deleted outright —
  the CM33's only GD32 transport is SPI now.
- (#1165) `da9292_v2n_m1_enable_deepx_rail()` and `da9292_v2n_base_init()`
  are deleted: both could drop a live CH2 rail. `da9292_ch2_sequence()`
  replaces them, following the same register sequence as U-Boot's
  `alp_deepx_rail_bringup()` with one deliberate mechanical difference:
  the patch clears `CH2_VSTEP`+`CH2_EN` in one combined read-modify-write,
  while the driver clears them as two separate read-modify-writes (`CH2_EN`
  first, then `CH2_VSTEP`, each read back) so a partial failure can never
  leave `CH2_VSTEP` cleared while `CH2_EN` is still set.
- The generated CM33 Zephyr board files (`e1m_v2n101_m33_sm`,
  `e1m_v2m101_m33_sm`) no longer claim RIIC8: `&i2c8` is `status =
  "disabled"` and the `alp-i2c0` alias is gone
  (`metadata/e1m_modules/v2n/supervisor-links.yaml`'s `brd_i2c` link flips
  to `status: disabled`).
- `metadata/pinmux/v2n.yaml` attributes `DEEPX_CORE_0P75_EN` (P64) and
  `DEEPX_PWR_EN_REQ` (P65) to `core: "a55"`.
- `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2m-deepx.dtsi`
  drops its `deepx-m1-reset-release-hog` (U-Boot is the sole `M1_RESET`
  owner); the P80/P95 PCIe-mux hogs are unchanged.
- `examples/v2n/v2n-rtc-multi-alarm` is retired -- it addressed the RTC
  over CM33 I2C, now forbidden (the on-module RV-3028-C7 is Linux's
  `/dev/rtc0`).
- `0001-clk-renesas-r9a09g056-keep-CM33-owned-RSCI7-on.patch` (retitled,
  was `...-RSCI7-RIIC8-on.patch`) drops its `riic_8_ckm`
  `DEF_MOD_CRITICAL` hunk -- Linux is RIIC8's real consumer now. The five
  `rsci_7_*` hunks (SCI7 SPI, still CM33-owned) are unchanged.

**Two unrelated BRD_I2C address corrections, bench-confirmed on
E1M-V2M103 (`alp i2c scan`/`i2cdetect`):** `CLK_5L35023B_I2C_ADDR_DEFAULT`
(`include/alp/chips/clk_5l35023b.h`) changed `0x68u` -> `0x69u` (the
fitted part straps `I2C_addr[1:0] = 01`, not the previously assumed
`00`), and `metadata/chips/tmp112.yaml`'s address entry changed `0x48`
-> `0x40` (the fitted TMP112DIDPWR straps `ADD0 = GND` to `0x40`, the
same class of bug already fixed for AEN #1978). Both are the actual
strapped address; every example and doc citing either is updated in the
same slice.

**`deepx_lpddr_0v85` strap resolved: `0x48` (#1163, #1845).** `0x48`
ACKs on `BRD_I2C` only after `P64` is driven high, and its VOUT register
reads back `0x5A` (= 0.85 V: 0.4 V + 90 x 5 mV), matching the role;
`0x44` (`deepx_ddr5_vdd`) and `0x4F` (`deepx_ddr5_vddq`) are present on
the same scan. `metadata/e1m_modules/E1M-V2M101.yaml`/
`E1M-V2M102.yaml`/`E1M-V2M103.yaml` change `address_7bit: "TBD"` ->
`"0x48"`; `docs/bring-up-v2n-m1.md` and `docs/soms/v2n-m1.md` are
updated to match and no longer warn against hardcoding `0x48`.

**`0005-i2c-rzg2l_riic-combined-register-read.patch`** makes every RIIC
register read on this driver (all nine RIIC instances) a single
repeated-START transaction. `dm_i2c_read()` always builds a register
read as two `struct i2c_msg`s (write-the-offset, then the paired read),
and the unpatched `riic_xfer()` ran each as its own independent
START..STOP transaction with a spurious STOP between them, instead of
the one combined transfer `riic_read_common()` already implements. The
fast path now packs the offset for any 1-4 byte width, which also fixes
the on-module 24C128 EEPROM's 2-byte-offset reads on RIIC0.
`riic_check_busy()` gains a one-shot BBSY-timeout recovery (clock SDA
free, full `ICE=0+IICRST` reset, retry), since `riic_ops` implements no
`dm_i2c` bus-recovery op, and `riic_wait_for_icsr2()`'s timeout
diagnostic now prints unconditionally instead of only in DEBUG builds.
Writes were unaffected (`dm_i2c_write()` always built one combined
message). This runs on silicon in the bench-passing chain described
below (v4..v6) -- it is not isolated as a fix for one symptom.

**`0006-rzv2n-dev-i2c-rzg2l_riic-p06-p07-pullup-clock-fix.patch`**
carries two independently bench-proven fixes for a repeated-START
arbitration-lost failure (`ICSR2 = 0x0a`, `STOP|AL`) that `0005` alone
did not clear, each sufficient on its own: (1) the SoC-internal pull-up
on P06/P07 (`PUPD_H` = `0x03030000`, matching what Linux already
programs -- U-Boot left it at the POR value of 0), and (2) a slower
RIIC8 SCL. `riic_set_clock()`'s CKS table (shared by every SoC this
driver builds for) is tuned for a 50 MHz RIIC input clock; R9A09G056/057
(RZ/V2N, RZ/V2H) run RIIC off a 100 MHz input, so the upstream value
(`CKS(3)` standard / `CKS(1)` fast) actually ran the bus at roughly
double rate. The nominal-100 MHz-correct value, `CKS(4)`/`CKS(2)`, still
lost bit 7 on long reads over the heavily-loaded BRD_I2C bus (~13
populated branches) from about byte 13 onward on bursts longer than
~12 bytes. `0006` ships one step slower still, `CKS(5)` standard /
`CKS(3)` fast, as deliberate margin: bench-confirmed, 32-byte bursts
come back clean and repeatable at `CKS(5)`. FAST-rate `CKS(3)` is
carried by the same margin reasoning but was not itself bench-tested
(only STANDARD/RIIC8 was exercised). External 2.2 kOhm pull-ups to the
1.8 V rail are fitted on this bus; the SoC-internal pull-up was
bench-confirmed required at the unpatched (faster) clock,
and is carried alongside `CKS(5)` -- either the pull-up or the slower
clock alone was sufficient to fix this arbitration-lost failure, and
both are kept.

`0006` also adds: `riic_read_common()` polls `ICCR1.SDAI` (SDA
released) before the repeated START, since restarting while a slave
still holds SDA low from its ACK is the arbitration-lost race seen on
the bench; `riic_check_busy()` clears stale `ICSR2` `AL|STOP|NACKF|
START` bits before the busy-bus check; `riic_wait_for_icsr2()`'s timeout
diagnostic now always prints `ICSR2`/`ICCR2`/`ICCR1`; and
`riic_i2c_raw_read()`'s `ICMR3.ACKBT`/`ICCR2.SP` register-set ordering
is fixed to set-then-read (matching the RIIC master-receive flowchart
and the already-correct `drivers/i2c/rz_riic.c riic_receive_data()`).
That last change is protocol correctness, not the cure for a
separately-observed bit-7 loss on long reads (bytes at offset
`0x0D`/`0x0E` losing bit 7 on a 16-byte burst) -- a systematic sweep
across CKS values (above) reproduced the identical corruption with and
without the reorder, so the reorder is kept on its own merits but the
bit-7 mechanism itself is unconfirmed pending a scope capture of the
affected bytes.

**DEEPX rail bench-PASSED on silicon, E1M-V2M103 (v6).**
`board_late_init()` ran for real and brought CH2 up on its own. Console:
`ALP: DA9292 programmed CTRL_01=0x01 VOUT_CH2=0x96/0x96` then `ALP:
DEEPX rail 0.75V up (PG)`. Read back after boot: `CTRL_01` `0x03`,
`VOUT_CH2` `0x96/0x96`, `STATUS_00` `0x03` (`CH2_PG` set, no UV/OV/OC),
`STATUS_01` `0x00`; the DEEPX bucks surfaced on `BRD_I2C` once `P64`
went high, all present and matching their known roles: `0x44` `0x82`,
`0x48` `0x5a`, `0x4F` `0x14`.

The PCIe step (`alp_deepx_pcie_bringup()`, patch `0001`) did not run on
this pass -- the on-module EEPROM manifest is blank, so the
EEPROM-manifest gate never opens; `CONFIG_ALP_E1M_DEEPX_RAIL` on its own
is enough to run the rail step, but the PCIe path still needs a
programmed manifest, unrelated to this patch. BENCH-PENDING: PCIe, once
a programmed EEPROM manifest is available to test against.

**Patch md5s (current):** `0002` `c546f00cabca346e335febd21ecbc440`,
`0004` `269fc80793f33514871704d3c1f4fc73`, `0006`
`79cce6934ecf6b205e5017866634d8bd`. `0004` applies cleanly on top of the
corrected `0002` and the meta-renesas PMIC-I2C-removal patch, against
`renesas-u-boot-cip` `bcf29d98`.

The `0005`/`0006` comments in
`meta-alp-sdk/recipes-bsp/u-boot/u-boot_%.bbappend` and the pull-up
comment in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-x-evk.dtsi` are
updated to match this state.
