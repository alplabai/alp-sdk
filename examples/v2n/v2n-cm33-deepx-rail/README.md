# v2n-cm33-deepx-rail

Sequence the DEEPX DX-M1 core rail (DA9292 CH2, `VDD_0P75`, 0.75 V) from
the CM33 system-manager, in **CM33-boot mode only**.

## Why a CM33-side sequencer exists at all

The RZ/V2N boot CPU is a hardware strap, not a software choice: pin
`BOOTSELCPU` (RZ/V2N HW manual R01UH1071EJ0110 Rev.1.10 Sec.1.9 Table
1.9-1) selects **LOW = CM33 cold boot, HIGH = CA55 cold boot**, driven by
ACT88760 GPIO5 (net `V2N_BOOT_CPU_SEL`); which GPIO5 level selects which
CPU is not yet recorded in `power-tree.yaml` (TBD). In the DEFAULT
(`a55_boot`) config U-Boot's `board_late_init()`
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
sequences this same rail on the A55, and CA55/Linux is thereafter the
sole master of RIIC8/BRD_I2C
(`metadata/e1m_modules/v2n/core-ownership.yaml`).

If `BOOTSELCPU` is instead strapped low, the CM33 cold-boots **first**
(from xSPI or SCIF download — the only two boot sources CM33-cold-boot
supports) and must release the CA55 itself, later. This app is what runs
during that window.

Ownership is **time-sliced, never concurrent**: this app masters RIIC8
only until it hands off to the CA55 (see "CA55 release" below); once the CA55
starts, U-Boot 0004 runs again as a warm, idempotent **verify** (its
program phase is a no-op when CH2 is already at target — no double
sequencing).

**Metadata status.** `power-tree.yaml` `boot_modes.cm33_boot` is still
`status: blocked` and `core-ownership.yaml` still gives RIIC8 and P64/P65 to
the A55 (standing 2026-09-24 decision). This example does not unblock that:
it needs a maintainer re-decision and the full slice `power-tree.yaml` lists.
It therefore **fails closed**: the app refuses to touch RIIC8 unless
`CONFIG_V2N_CM33_BOOT_CONFIRMED=y` is set, on a unit strapped for `cm33_boot`.
`check_amp_pad_claims.py` reads committed board dts, not this overlay.
TODO(alp-sdk#2289): replace the build-time acknowledgement with a runtime
`BOOTSELCPU` read once the register is cited from the manual.

## What it does

1. Opens BRD_I2C (`alp_i2c_open`, bus 0 -> the `alp-i2c0` alias this
   app's own board overlay defines) and the two DEEPX sequencing GPIOs,
   P64 (`DEEPX_CORE_0P75_EN`, output) and P65 (`DEEPX_PWR_EN_REQ`,
   input) — via `alp_gpio_open(0)` / `alp_gpio_open(1)`, resolved through
   this app's own 2-entry `alp,pin-array` (these are SoM-internal pads,
   not E1M edge pins, so they carry no canonical positional index).
2. `da9292_init()` + `da9292_set_limits()` against the generated
   `V2N_M1_POWER_DA9292_CH_LIMITS_INIT` table (V2M101 is the `v2n-m1`
   family — the only one with the DEEPX DX-M1 add-on).
3. `da9292_ch2_sequence()` — the SAME portable chip-driver function
   U-Boot 0004's C sequence mirrors step for step: program CH2 to
   0.75 V (skipped on a warm reboot if already there), poll
   `DEEPX_PWR_EN_REQ`, enable CH2, poll `CH2_PG`, then drive
   `DEEPX_CORE_0P75_EN` high and re-confirm PG.
4. Prints the result and which step it stopped at on failure.

## Board overlay

`boards/alp_e1m_v2m101_m33_sm_r9a09g056n48gbg_cm33.overlay` re-enables
`&i2c8` (disabled on the DEFAULT AMP board config) and defines this
app's own `alp,pin-array`. **Do not** copy this overlay into any other
CM33 example — RIIC8/BRD_I2C stays Cortex-A55/Linux-exclusive outside
this one CM33-boot pre-handoff window.

## CA55 release (`CONFIG_V2N_CM33_RELEASE_CA55`, default off)

After `da9292_ch2_sequence()` returns `ALP_OK` (and only then), the app can
call `v2n_cm33_release_ca55()` (`src/ca55_release.c`): it writes the CA55
core 0 reset vector (`SYS_ACPU_CFG_RVAL0/RVAH0`, CM33-staged BL2) and runs
the CPG cold-reset release of HW manual Table 2.2-7. BL2 keeps training DDR;
no OTP is involved. Decision and register facts:
[alp-sdk#2289](https://github.com/alplabai/alp-sdk/issues/2289).

Not done yet, so the helper fails closed with `ALP_ERR_NOSUPPORT` and the
CA55 stays held: the AWO to ALL_ON power-domain entry (Table 4.5-4), whose
register offsets are not in the tree, and the `CPG_RSTMON_0` CA55 bit mask.
Both are marked `TODO(alp-sdk#2289)` in the source, not guessed. The BL2
staging address (`CONFIG_V2N_CA55_BL2_ENTRY_ADDR`) is also undecided. Bench
steps 1-6 of the issue are still open.

## Build (native_sim, build-only)

Same pattern as every other V2N CM33 example: native_sim has no RZ/V2N
RIIC8 (or this app's board overlay), so the twister job checks the app
compiles + links, not that it runs. Nothing here has been run on silicon
yet.

## Build (real CM33 board, alplab-gw)

```sh
west build -b alp_e1m_v2m101_m33_sm_r9a09g056n48gbg_cm33 \
    examples/v2n/v2n-cm33-deepx-rail
```
