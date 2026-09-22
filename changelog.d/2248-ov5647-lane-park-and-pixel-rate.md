### Fixed — OV5647: park the CSI-2 lanes at LP-11 and correct the PLL-derived pixel rate (#2248)

`2248-ov5647-root-cause.md` found two real defects in the vendored,
upstream-pending `zephyr/drivers/video/ov5647.c` (backport of
zephyrproject-rtos/zephyr#119301) but left both unpatched, because the file's
header forbids divergent local patches on a verbatim backport. The maintainer
has now explicitly overruled that for these two fixes — an authorized,
recorded divergence, not an oversight — and the header is updated to say so
and to gate the file's eventual retirement on the fixes being re-applied or
confirmed present upstream first.

**Defect 1 — no LP-11 presented, so a Stop-state-gated CSI-2 receiver could
never open the link.** The driver left the sensor in bare software standby
(`0x0100` = `0x00`) after init, in which `CSI_PHY_STOPSTATE` reads
`0x00000000` — no lane at LP-11 — bench-measured on an E1M-AEN803 (serial
2026W36-0001) / E1M-EVK (hw_rev 2626-r2) with a DesignWare CSI-2 receiver.
Writing `0x0100` = `0x01` (sensor running) and then `0x4800` = `0x25`
(`CLOCK_LANE_GATE|BUS_IDLE|CLOCK_LANE_DISABLE`), `0x4202` = `0x0f`, `0x300d` =
`0x01` — in that order, matching mainline Linux's `ov5647_power_on()` calling
`ov5647_stream_stop()` under the comment "Stream off to coax lanes into LP-11
state" — makes the same register read back `0x00000001` steadily (50/50
samples at 20 ms; the module under test has a hardware fault on DATA_1/CLK,
limiting it to bit0, which does not affect this fix — a healthy module should
reach `0x00010003`). A new `ov5647_lane_park()` helper applies this sequence,
called at the end of `ov5647_init()` so the sensor is left parked rather than
in bare standby, and again from `ov5647_set_stream(dev, false, ...)` so a
stopped stream still presents LP-11 for the next open.
`ov5647_set_stream(dev, true, ...)` unparks first: `0x4800` = `0x04`
(`BUS_IDLE`), `0x4202` = `0x00`, `0x300d` = `0x00`, then the existing
`0x0100` = `0x01`.

**Defect 2 — the declared pixel rate was 2.1x too low, steering the D-PHY
frequency-bin lookup wrong.** `OV5647_PIXEL_RATE(clk)` was `(clk) * 10 / 3` =
83333333 for the nominal 25 MHz XVCLK, derived from a datasheet fps figure
rather than the PLL. The driver never programs the PLL, so it stays at its
power-on values, read back on the same bench unit: `0x3034` = `0x1a`
(bits[3:0] = 0xa, 10-bit mode), `0x3035` = `0x11` (bits[7:4] system-clock
divider 1, bits[3:0] MIPI divider 1), `0x3036` = `0x69` (multiplier 105),
`0x3037` = `0x03` (bits[3:0] pre-divider 3, bit4 root divider 0 = /1). That
gives VCO = 25 MHz / 3 x 105 = 875 MHz (875 Mbps/lane), and pixel rate =
875e6 x 2 lanes / 10 bpp = 175000000, now expressed as a derived value
(`OV5647_PLL_PREDIV`, `OV5647_PLL_MULT`, `OV5647_PLL_ROOT_DIV`,
`OV5647_MIPI_LANES`, `OV5647_MIPI_BPP`) with the arithmetic in a comment
rather than a bare constant. Zephyr's fallback then computes link = pixel_rate
x bpp / (2 x lanes) = 437500000 Hz = 875 Mbps, selecting D-PHY bin
`{900, 0x29}` instead of the wrong `{450, 0x16}`.

**Not confirmed on silicon**: the pixel-rate arithmetic assumes the OV5647
shares the OV564x family's PLL structure and pre-divider encoding — inferred,
not taken from an OV5647-specific PLL block diagram — because the only module
on the bench has the DATA_1/CLK hardware fault noted above and could not give
a live D-PHY link-rate measurement. At 875 Mbps the CSI pixel clock request
lands on the 200 MHz clamp branch in `zephyr/drivers/video/video_csi_dw.c`.
Both caveats are recorded as comments in `ov5647.c` itself for the next
person to re-verify on a healthy module.
