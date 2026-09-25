### Fixed — U-Boot fixes the on-module clock generator's SE1/SE3 OTP routing so the SoC RTC starts (#2045)

The on-module Renesas 5L35023B programmable clock generator (RIIC8/BRD_I2C,
7-bit `0x69`) ships an OTP image whose single-ended output routing is wrong
for this SoM: SE1 (feeds the SoC RTXIN and the Wi-Fi module's 32k LPO input)
outputs `24.576 MHz` instead of `32.768 kHz`, and SE3 (audio clock) outputs
`22.5792 MHz` instead of `24.576 MHz`. Bench-confirmed on E1M-V2M103 board
#1: with the as-shipped OTP values the SoC RTC (RTCA-3) fails to start
(`error -ETIMEDOUT: Failed to setup the RTC!`).

Two volatile register writes fix it (5L35023B datasheet register map,
"Byte 36: SE1 and DIV4 control" = reg `0x24`, "Byte 33: SE3 and DIFF1
Control Register" = reg `0x21`):

* register `0x24` `0x9c` (`1001_1100`) -> `0x8e` (`1000_1110`): clears bit 4
  `SE1_Freerun_32K` (`1`->`0` — 0 means SE1 is sourced from the 32.768 kHz
  DCO instead of free-running) and sets bit 1 `DIV4_CH3_EN` (`0`->`1` — DIV4
  channel 3 output enabled). Bit 7 `I2C_PDB`, bit 6 `Ref_free_run`, bit 5
  `free_run_output_config`, bit 3 `SE1_CLKSEL1`, bit 2 `REF_EN` and bit 0
  `DIV4_CH2_EN` are all unchanged.
* register `0x21` `0x80` (`1000_0000`) -> `0xc0` (`1100_0000`): sets bit 6
  `SE3_CLKSEL1` (`0`->`1` — SE3 now sourced from DIV4 = PLL2 `122.88/5` =
  24.576 MHz, was DIV2). Every other bit is unchanged, including both
  `DIFF1_CMOS2_FLIP`/`DIFF2_CMOS2_FLIP` bits that also live in this
  register. Neither write touches the separate DIFF1/DIFF2 registers
  (`0x22`/`0x23`) -- those are untouched.

With them applied, the SoC RTC counts at 32.768 kHz (bench-verified against
an NTP-slewed reference, ~±100 ppm). Both are OTP-shadow registers -- the
writes take effect immediately but **revert on power-cycle** (the OTP
itself cannot be re-burned in-system), so they must be re-applied on every
boot.

`meta-alp-sdk/recipes-bsp/u-boot/u-boot/0007-rzv2n-dev-ALP-E1M-clkgen-otp-fixup.patch`
adds `alp_clk5l_fixup()`, run first in `board_late_init()` -- ahead of
and independent of the `0004` DEEPX rail step -- for the whole rzv2n-family
(this clock generator is present on every V2N/V2M SoM, not just
DEEPX-populated V2M units). Guarded to single-byte reads only (matching the
constraint `0005`/`0006` document for this RIIC family) and to writing only
these two registers: register `0x00` (General Control) must first read
`0xa0` (OTP-burned, `I2C_addr[1:0]=01` -> 7-bit `0x69`). Register `0x24`
is then handled first, then `0x21` -- each independently: its as-shipped
OTP value writes the fix and reads it back, its already-fixed value skips,
and any other readback is left untouched and only reported (rather than
requiring both registers to read the OTP pair before either is touched,
which would report a half-applied prior run as "unexpected" instead of
completing it). If `0x24`'s write/readback fails, `0x21` is never touched.
Each write is read back before the next register is touched.

Logs one line: `ALP: 5L35023B clock: SE1 32.768 kHz, SE3 24.576 MHz
(0x24=0x8e 0x21=0xc0)` on success, or a per-register `... already ... --
skip` / `... is neither ... -- not touching` line otherwise.

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
second, SoC-internal timebase, not a replacement. Because `&rtc` (the SoC
RTC, `rtc-rtca3`) is now enabled, it probes on every boot alongside
`rv3028`, and Linux's `/dev/rtcN` numbering follows probe order, not the
`aliases` block -- without a fixed id the SoC RTC (which probes before
`rv3028` on this bus) would take `/dev/rtc0` and displace `rv3028` to
`/dev/rtc1`, breaking `RTC_HCTOSYS`. A new `rtc1 = &rtc;` alias pins the
SoC RTC to `/dev/rtc1` so `rv3028` keeps `/dev/rtc0`. Stale "no crystal,
nothing to enable" comments in `e1m-x-evk.dtsi` and `rv3028-rtc.cfg` are
corrected to match.

This is a runtime workaround; **production builds should use a Renesas
factory dash code that carries the corrected OTP image** instead of relying
on the U-Boot fixup.

`docs/soms/v2n.md` gains an "On-module clock-generator fixup" section (plus
a corrected "Real-time clock" section) and `docs/bring-up-v2n-m1.md` cross-
references it from the DEEPX-rail step, noting the fixup's log line appears
first and is unrelated to DEEPX sequencing.

**Patch md5 (current):** `0007` `422cb294103c4a3878bd7ad43150e35b`. Applies
cleanly on top of `0006`, against `renesas-u-boot-cip` `bcf29d98` plus the
meta-renesas PMIC-I2C-removal patch and `0001`-`0006`. Cross-built
(`ARCH=arm CROSS_COMPILE=aarch64-linux-gnu- rzv2n-dev_defconfig` +
`no-dirty-version.cfg`/`gigadevice-xspi.cfg`/`deepx-rail.cfg`/
`fdtfile-v2m.cfg`); `strings u-boot | grep 'ALP: 5L35023B'` confirms all
eleven log lines link, `u-boot.bin` md5 `9f23a19d1afdf69179319921b6327470`.
Not yet flashed/bench-booted -- register-write values are bench-proven
(2026-09-24, E1M-V2M103 board #1) but this patch's own build was verified
as a clean cross-compile + string-link only, not (yet) an on-silicon boot.
