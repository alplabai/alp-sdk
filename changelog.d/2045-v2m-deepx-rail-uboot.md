### Added — U-Boot sequences the V2N-M1 DEEPX 0.75 V core rail before releasing `M1_RESET` (#2045)

The V2N-M1 SoM's DEEPX DX-M1 core rail (DA9292 CH2, 0.75 V) had no sequencing
step anywhere: `board_late_init()` released `M1_RESET` and enabled the PCIe
mux purely on the on-module EEPROM manifest match, with nothing bringing CH2
up first. The DA9292-AROVx OTP variant this SoM uses boots with
`CH2_VSTEP=1` (doubled voltage range, `PMC_CTRL_01` reset `0x80`); releasing
`M1_RESET` before CH2 is confirmed at 0.75 V and power-good risked an
over-voltage event on the DEEPX core rail the instant the chip started
drawing current, not merely a brownout.

`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`
adds `alp_deepx_rail_bringup()`, run in `board_late_init()` before the
existing `alp_deepx_pcie_bringup()` (patch 0001). It programs CH2 to 0.75 V
over DM I2C bus 8 (RIIC8/BRD_I2C, DA9292 at 0x1E), verifies every write by
read-back, is idempotent across a warm reboot (skips re-programming a
channel already correctly set, so it never clears a validly-up `CH2_EN`),
then only enables the channel and releases P64 (`DEEPX_CORE_0P75_EN`) after
polling P65 (`DEEPX_PWR_EN_REQ`) and confirming `CH2_PG` with no UV/OV/OC.
`alp_deepx_pcie_bringup()` now runs only when the rail step succeeds —
`M1_RESET` is never released with the rail down. Gated on the new
`CONFIG_ALP_E1M_DEEPX_RAIL` (default `n`, wired via `deepx-rail.cfg` for the
V2M MACHINEs only) OR the existing EEPROM-manifest check, so either path
alone is sufficient and V2N base images touch nothing on the DA9292.

**Maintainer decision carried through in full:** Cortex-A55/Linux is the
sole master of RIIC8/BRD_I2C; the CM33 must never master it or claim
P64/P65. This retires the CM33-side attempt at the same job:

- `src/zephyr/v2n_power_mgmt.c` / `.h`, `CONFIG_ALP_SDK_V2N_POWER_MGMT`, and
  the V2N supervisor's BRD_I2C transport
  (`CONFIG_ALP_SDK_V2N_SUPERVISOR_I2C_BUS_ID`/`_I2C_ADDR`/`_I2C_BITRATE_HZ`,
  `alp_z_v2n_supervisor_brd_i2c_acquire`/`_release`) are deleted outright —
  the CM33's only GD32 transport is SPI now.
- `chips/da9292`'s driver stays, but `da9292_v2n_m1_enable_deepx_rail()` /
  `da9292_v2n_base_init()` are documented diagnostic/read-only-only; nothing
  in alp-sdk may call them as part of a boot flow.
- The generated CM33 Zephyr board files (`e1m_v2n101_m33_sm`,
  `e1m_v2m101_m33_sm`) no longer claim RIIC8: `&i2c8` is `status =
  "disabled"` and the `alp-i2c0` alias is gone
  (`metadata/e1m_modules/v2n/supervisor-links.yaml`'s `brd_i2c` link flips
  to `status: disabled`).
- `metadata/pinmux/v2n.yaml` attributes `DEEPX_CORE_0P75_EN` (P64) and
  `DEEPX_PWR_EN_REQ` (P65) to `core: "a55"` so a CM33 board file can't claim
  them.
- `meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2m-deepx.dtsi` drops
  its `deepx-m1-reset-release-hog` (U-Boot is the sole `M1_RESET` owner);
  the P80/P95 PCIe-mux hogs are unchanged.
- `examples/v2n/v2n-rtc-multi-alarm` is retired — it addressed the RTC over
  CM33 I2C, now forbidden (the on-module RV-3028-C7 is Linux's `/dev/rtc0`,
  per the RIIC8-ownership fix that also lands on this branch).
- The kernel patch
  `0001-clk-renesas-r9a09g056-keep-CM33-owned-RSCI7-on.patch` (retitled, was
  `...-RSCI7-RIIC8-on.patch`) drops its `riic_8_ckm` `DEF_MOD_CRITICAL`
  hunk — Linux is RIIC8's real consumer now, so `clk_disable_unused`
  correctly leaves it alone on its own. The five `rsci_7_*` hunks (SCI7
  SPI, still CM33-owned) are unchanged.

The U-Boot patch shipped here (`0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
is the `git format-patch`-regenerated version, not this branch's first
draft — three real defects a synthetic dry-run apply could not catch were
found and fixed before landing:

- the P65 (`DEEPX_PWR_EN_REQ`) input read was reading the P output-latch
  register (last value *written*), not the live pin level. Reads it from
  the dedicated PFC terminal-input register (`PFC_BASE+0x0820+n`) instead
  -- bench-confirmed against a real input bit.
- every program-phase abort now forces P64 low / `M1_RESET` asserted via
  a shared `alp_deepx_rail_safe_off()` helper, not only the enable-phase
  failures that already did this inline.
- **Hardening:** `CH2_VSTEP` is re-checked immediately before the
  `CH2_EN` write (not just earlier in the program phase) and aborts if
  it is ever found set -- the exact over-voltage condition this whole
  sequence exists to prevent.
- The PG-timeout failure message now also names a possible EN2-gates-CH2
  hardware wiring question (`CH2 may require EN2/P64 high before PG --
  see bring-up doc`) -- see `docs/bring-up-v2n-m1.md` §2 for what to
  check if it appears on real silicon.

**Two unrelated BRD_I2C address corrections, bench-confirmed on the same
pass (E1M-V2M103, `alp i2c scan`/`i2cdetect`):** the public macro
`CLK_5L35023B_I2C_ADDR_DEFAULT` (`include/alp/chips/clk_5l35023b.h`)
changed `0x68u` -> `0x69u` — the fitted part straps `I2C_addr[1:0] = 01`,
not the previously assumed `00`; and `metadata/chips/tmp112.yaml`'s
address entry changed `0x48` -> `0x40` — the fitted TMP112DIDPWR
(X2SON-5) straps `ADD0 = GND` to `0x40`, the same class of bug already
fixed for AEN (#1978). Both are the actual strapped address, not a
board defect; every example and doc citing either address is updated in
the same slice.

**Compile proof:** `0004` applies cleanly (verified on `alplab-gw`, git
`apply --check` + `apply`) on top of the CORRECTED `0002` (md5
`c546f00cabca346e335febd21ecbc440`, PR #2280) and the meta-renesas
PMIC-I2C-removal patch, in that order, against `renesas-u-boot-cip`
`bcf29d98`. It does NOT apply against this branch's own uncorrected
`0002` -- that patch fails to apply outright on a clean `bcf29d98`
checkout (its "gigadevice" hunks are genuinely broken, not merely
conflicting with `0004`), confirming #2280 must land first. Compiled
and link-verified in WSL Ubuntu-22.04 (fresh `bcf29d98` + the
PMIC-removal patch + `0001` + corrected `0002` + `0004`, md5
`269fc80793f33514871704d3c1f4fc73`; `rzv2n-dev_defconfig` +
`deepx-rail.cfg` + `fdtfile-v2m.cfg` + `no-dirty-version.cfg` +
`gigadevice-xspi.cfg`; `aarch64-linux-gnu` gcc 11.4.0): build `rc=0`,
0 warnings, `strings u-boot | grep '^ALP:'` shows every DA9292/DEEPX
rail string including `ALP: DEEPX rail 0.75V up (PG)` and the new
EN2/P64 message. Still BENCH-PENDING: no run on a V2M bench unit yet;
the 4 GB V2M103 bench FIP pack is still pending that session. The md5
changed from the prior `59f81f3f9ea3812d99bd3c476ede2b86` only by a
present-tense rewrite of the commit-message prose (retired-module and
bug-history phrasing); the diff hunks are byte-identical, so this
compile proof still covers the patch content unmodified.
