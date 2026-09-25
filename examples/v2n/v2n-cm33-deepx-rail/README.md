# v2n-cm33-deepx-rail

Sequence the DEEPX DX-M1 core rail (DA9292 CH2, `VDD_0P75`, 0.75 V) from
the CM33 system-manager, in **CM33-boot mode only**.

## Why a CM33-side sequencer exists at all

The RZ/V2N boot CPU is a hardware strap, not a software choice: pin
`BOOTSELCPU` (RZ/V2N HW manual R01UH1071EJ0110 Rev.1.10 Sec.1.9 Table
1.9-1) selects **LOW = CM33 cold boot, HIGH = CA55 cold boot**, driven by
ACT88760 GPIO5 (net `V2N_BOOT_CPU_SEL`). The power sequencer's own CMI
drives that net **HIGH by default**, ~8.6 ms after `MODULE_EN` — so
CA55-cold-boot is the power-on default, and in that DEFAULT config
U-Boot's `board_late_init()`
(`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`)
sequences this same rail on the A55, and CA55/Linux is thereafter the
sole master of RIIC8/BRD_I2C
(`metadata/e1m_modules/v2n/core-ownership.yaml`).

If `BOOTSELCPU` is instead strapped low, the CM33 cold-boots **first**
(from xSPI or SCIF download — the only two boot sources CM33-cold-boot
supports) and must release the CA55 itself, later. This app is what runs
during that window.

Ownership is **time-sliced, never concurrent**: this app masters RIIC8
only until it hands off to the CA55 (see the TODO below); once the CA55
starts, U-Boot 0004 runs again as a warm, idempotent **verify** (its
program phase is a no-op when CH2 is already at target — no double
sequencing). `metadata/e1m_modules/v2n/power-tree.yaml`'s
`boot_modes.cm33_boot` and `core-ownership.yaml`'s `boot_mode_core`
qualifier record this so `scripts/gen_power_tree.py`'s `cross_check()`
still rejects a real dual-master (non-time-sliced) config.

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

## TODO: CA55 release

This app deliberately stops after a successful rail-up instead of
releasing the CA55: doing that needs the actual RZ/V2N CPU-reset-control
register(s), which nobody has confirmed against the hardware manual or
bench-verified yet. Tracked in
[alp-sdk#2289](https://github.com/alplabai/alp-sdk/issues/2289) — do not
add a register write here without a hardware-manual citation.

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
