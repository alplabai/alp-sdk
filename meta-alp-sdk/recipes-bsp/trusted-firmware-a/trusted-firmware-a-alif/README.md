# Alif TF-A patches (carried, not yet wired into a recipe)

`0001-alif-make-the-console-uart-and-its-pads-build-knobs.patch` applies to
[`trusted-firmware-a_alif`](https://github.com/alifsemi/trusted-firmware-a_alif)
branch `alif_lts-v2.10.8`, commit `59a39e03986a8b1961895181a3351abcfd5200f9`
— the tree the Cortex-A32 boot chain was built from (#1979).

## What it changes, and why it is not optional for an Alp carrier

Upstream pins the console to the Alif DevKit's UART2 in **two independent
places**:

* the register base — `PLAT_ALIF_BOOT_UART_BASE` / `PLAT_ALIF_RUN_UART_BASE`
  `= UL(0x4901A000)` in
  `plat/alif/board/devkit_e7/common/include/platform_def.h`
* the pads — `devkit_e7_pinmux_init()` in
  `plat/alif/board/devkit_e7/sp_min/devkit_e7_sp_min_setup.c`, which calls
  `pinconf_set(PORT_1, PIN_0, PINMUX_ALTERNATE_FUNCTION_1, ...)` and
  `pinconf_set(PORT_1, PIN_1, ...)`

Moving only the register base yields a console that initialises, runs, and is
routed nowhere — no output and no error. That is what the first flash on the
Alp E1M-AEN EVK produced, and it cost a bench session to diagnose.

The patch turns both into build knobs (`ALIF_CONSOLE_UART_BASE`, plus
`ALIF_CONSOLE_RX_PORT` / `_RX_PIN` / `_TX_PORT` / `_TX_PIN` / `_PIN_FUNC`),
defaulting to the DevKit values so a stock `devkit_e7` build is unchanged.

## Status — observed

Verified by direct `make` against the vendor tree, not by inspection:

* applies cleanly to a pristine clone of `alif_lts-v2.10.8`
  (`git apply --check` passes);
* the patched tree builds BL32 for the carrier with the knobs set — the string
  `4901d000` appears in the resulting image and `4901a000` does not;
* a stock `devkit_e7` build with no knobs set still succeeds.

## Why there is no `.bbappend` here yet

There is deliberately no bbappend in this directory. A bbappend whose recipe
does not exist is a **hard bitbake error** by default — `meta-alp-sdk` does not
set `BB_DANGLINGAPPENDS_WARNONLY` anywhere — so adding one now would red every
build that includes this layer, including the Renesas V2N build that works
today. No Alif TF-A recipe exists in any layer in this repo, and no bitbake or
Yocto build has ever been run for Alif here (#1968, #1971).

There is also a naming trap to solve before one is written: bitbake derives
`PN` from the filename up to the first underscore, so the only Alif TF-A recipe
on record (`trusted-firmware-a.bb`, from `meta-alif-ensemble`) has
`PN = trusted-firmware-a` — the same `PN` as the Renesas recipe this layer
already appends. A future bbappend has to disambiguate by machine override, not
by filename.

Until then the patch is consumed directly by the A32 bring-up build, which
clones the vendor tree at the pinned commit above and applies it.
