### Added — U-Boot fixes the on-module clock generator's SE1/SE3 OTP routing so the SoC RTC starts (#2045)

The on-module Renesas 5L35023B programmable clock generator (RIIC8/BRD_I2C,
7-bit `0x69`) ships an OTP image whose single-ended output routing is wrong
for this SoM: SE1 (feeds the SoC RTXIN and the Wi-Fi module's 32k LPO input)
outputs `24.576 MHz` instead of `32.768 kHz`, and SE3 (audio clock) outputs
`22.5792 MHz` instead of `24.576 MHz`. Bench-confirmed on E1M-V2M103 board
#1: with the as-shipped OTP values the SoC RTC (RTCA-3) fails to start
(`error -ETIMEDOUT: Failed to setup the RTC!`).

Two volatile register writes fix it: register `0x24` `0x9c` -> `0x8e` (SE1 =
32.768 kHz DCO; keeps `I2C_PDB`=1, `REF_EN`=1, `DIV4_CH3_EN`=1), then
register `0x21` `0x80` -> `0xc0` (SE3 from DIV4 = PLL2 `122.88/5` =
24.576 MHz). With them applied, the SoC RTC counts at 32.768 kHz
(bench-verified against an NTP-slewed reference, ~±100 ppm). Both are
OTP-shadow registers -- the writes take effect immediately but **revert on
power-cycle** (the OTP itself cannot be re-burned in-system), so they must
be re-applied on every boot.

`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0007-rzv2n-dev-ALP-E1M-clkgen-otp-fixup.patch`
adds `alp_clk5l_fixup()`, run first in `board_late_init()` -- ahead of
and independent of the `0004` DEEPX rail step -- for the whole rzv2n-family
(this clock generator is present on every V2N/V2M SoM, not just
DEEPX-populated V2M units). Guarded to single-byte reads only (matching the
constraint `0005`/`0006` document for this RIIC family) and to writing only
these two registers: register `0x00` (General Control) must first read
`0xa0` (OTP-burned, `I2C_addr[1:0]=01` -> 7-bit `0x69`), and `0x24`/`0x21`
must read the exact as-shipped OTP pair (`0x9c`/`0x80`) before anything is
written; any other readback (already fixed, a differently configured part,
or a communication error) is left untouched and only reported. Each write
is read back before the next register is touched.

Logs one line: `ALP: 5L35023B clock: SE1 32.768 kHz, SE3 24.576 MHz
(0x24=0x8e 0x21=0xc0)` on success, or a `... already set ... -- skip` /
`... unexpected ... -- not touching` line otherwise.

**Kernel devicetree (`meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n-som.dtsi`):**
the SoC RTC (`&rtc`) is now enabled and its `rtc_clk` fixed-clock node set
to the corrected `clock-frequency = <32768>` -- the previously deployed dtb
carried `clock-frequency = <0>` (the vendor SoC dtsi's own unset-placeholder
default; no ALP override had ever set it, on the earlier, incomplete
understanding that RTXIN had no reference at all). RTXIN in fact has no
*discrete crystal*, but it is fed by SE1 of the on-module clock generator
once U-Boot corrects it, and DT enablement now depends on that runtime fix
having already run before Linux probes `&rtc`. The on-module RV-3028-C7
remains the RTC of record (`rtc0` alias, unchanged) -- the SoC RTC is a
second, SoC-internal timebase, not a replacement. Stale "no crystal, nothing
to enable" comments in `e1m-x-evk.dtsi` and `rv3028-rtc.cfg` are corrected
to match.

This is a runtime workaround; **production builds should use a Renesas
factory dash code that carries the corrected OTP image** instead of relying
on the U-Boot fixup.

`docs/soms/v2n.md` gains an "On-module clock-generator fixup" section (plus
a corrected "Real-time clock" section) and `docs/bring-up-v2n-m1.md` cross-
references it from the DEEPX-rail step, noting the fixup's log line appears
first and is unrelated to DEEPX sequencing.

**Patch md5 (current):** `0007` `792b599bd99c0cd7c35e6a7aa0c97108`. Applies
cleanly on top of `0006`, against `renesas-u-boot-cip` `bcf29d98` plus the
meta-renesas PMIC-I2C-removal patch and `0001`-`0006`. Cross-built
(`ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- rzv2n-dev_defconfig` +
`no-dirty-version.cfg`/`gigadevice-xspi.cfg`/`deepx-rail.cfg`/
`fdtfile-v2m.cfg`); `strings u-boot | grep 'ALP: 5L35023B'` confirms all
eight log lines link. Not yet flashed/bench-booted -- register-write values
are bench-proven (2026-09-24, E1M-V2M103 board #1) but this patch's own
build was verified as a clean cross-compile + string-link only, not (yet)
an on-silicon boot.
