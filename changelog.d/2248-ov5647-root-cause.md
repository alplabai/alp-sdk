### Fixed — OV5647 bring-up: the driver programmed no PLL and no MIPI-TX pad-drive registers; the module was never at fault (#2248)

**CORRECTION to the earlier "root-caused to a module fault" entry in this
file**: that conclusion is retracted. The InnoMaker CAM-OV5647 module is
healthy; the earlier bench finding (`CSI_PHY_STOPSTATE` reading `DATA_0`
50/50 and `DATA_1`/`CLK` 0/50, with an OV9281 on the same cable and
connector reaching `0x00010003`) measured the vendored driver's power-on
LP-TX drive strength, not a silicon defect. With the driver programming no
PLL either, the sensor free-ran at 875 Mbps/lane and a 175 MHz pixel clock
against a receiver binned for 416.67 Mbps — a 2.1x mismatch giving
`ERRSOTSYNCHS` on both data lanes on every burst, with the clock lane
still locking (a clock lane locks far outside the range a data lane can
sync SoT in).

Fixed on an E1M-AEN803 serial 2026W36-0001 / E1M-EVK hw_rev 2626-r2, RAW10
640x480 (bench run 52): `PHY_FATAL` 7712 -> 0, `capture ALP_OK`, 60 frames
at 15.96 fps, frame buffer md5 `61cdf7a021584f4bfaa2ae282c7b2256` with
`a5=0/614400` (every pre-fill byte overwritten), `CAM_FRAME_ADDR`
advancing `0x02000080` -> `0x020960c0`. `zephyr/drivers/video/ov5647.c`
now writes eight registers (`0x3017 0x3035 0x3036 0x3037 0x303c 0x301c
0x301d 0x3106`) in software standby, before the existing lane-park fix
runs — the PLL dividers latch at the `0x0103` software reset, not on
standby exit, so the same writes issued after the park update the
register file without moving the running PLL (a bench run 51 ordering
mistake). PLL values are matched to mainline Linux's own declared
constants (prediv 3, multiplier 105, sysdiv 2 -> 437.5 Mbps/lane, matching
mainline's `link_freq` 218750000). `0x3017` (LP TX pad drive) is
bench-bisected: reset `0x10` -> no lane reaches Stop state; `pgm_lptx =
01` -> `DATA_0` only; `10` -> `+DATA_1`; `11` -> `+CLK`, reaching
`CSI_PHY_STOPSTATE` = `0x00010003` with `0x3017` = `0xf0` — one step
higher drive than mainline's `0xe0`, which never brings the clock lane to
Stop state; mainline's own receiver does not gate on that lane but ours
does. This is a second AUTHORIZED divergence on top of the existing
lane-park fix, recorded the same way in the file header.

`OV5647_PIXEL_RATE` is also corrected, not "disproven" as this file
previously said: a prior attempt to fix it in isolation to a PLL-derived
175000000 was reverted, and a follow-up investigation concluded that
attempt was wrong and the unmodified upstream value was "approximately
correct". Both conclusions are now retracted: 175000000 was actually
right about the *power-on* link rate, and "it doubled the programmed VTS"
was the correct consequence of a doubled pixel clock, not evidence against
it. The defect was that the PLL was never *written* at all. The macro now
derives from the same PLL constants the register table programs
(`OV5647_PLL_PREDIV`/`MULT`/`SYS_DIV`), giving 87500000 at 25 MHz XVCLK —
matching mainline's own declared `pixel_rate` for this PLL.

**Honest limits, not established by this fix**: which of the eight
run-52 registers are individually load-bearing — only `0x3017` is
independently bisected; the rest are shipped together because, matched to
mainline's values, that is what bench-proved. `0x4837` (PCLK period)
stays at its power-on `0x15` (mainline writes `0x19`); safe at 437.5 Mbps,
not changed in the working run. `0x3017` = `0xf0` has no characterised
drive-strength margin. `OV5647_PIXEL_RATE`'s correction is fixed at 10bpp
(RAW10, the only bench-proven format); the 8-bit (SBGGR8) path would need
a different pixel rate (109375000) that this change does not derive or
verify — flagged in the driver, not fixed. `CSI_PHY_RX`/`CSI_PHY_STOPSTATE`
read byte-identically in the failing and working runs at every capture
point and should not be used alone as a bring-up gate.

A new runtime regression test asserts the eight registers are written
*while the sensor is in software standby and before the park sets*
`0x0100` = `0x01`, not just that the values are eventually present — a
values-only check would pass the exact reordered variant that cost bench
run 51.

`docs/camera-shields.md` records the corrected root cause, the run-52
evidence, and all of the above limits. The OV5647 shield is now
bench-verified for RAW10; RAW8 is unverified.
