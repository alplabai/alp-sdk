### Fixed — OV5647: park the CSI-2 lanes at LP-11; pixel-rate fix reverted pending further investigation (#2248)

`2248-ov5647-root-cause.md` found two claimed defects in the vendored,
upstream-pending `zephyr/drivers/video/ov5647.c` (backport of
zephyrproject-rtos/zephyr#119301) but left both unpatched, because the file's
header forbids divergent local patches on a verbatim backport. The maintainer
has now explicitly overruled that for the one real fix — an authorized,
recorded divergence, not an oversight — and the header is updated to say so
and to gate the file's eventual retirement on the fix being re-applied or
confirmed present upstream first.

**Fixed — no LP-11 presented, so a Stop-state-gated CSI-2 receiver could
never open the link.** The driver left the sensor in bare software standby
(`0x0100` = `0x00`) after init, in which `CSI_PHY_STOPSTATE` reads
`0x00000000` — no lane at LP-11 — bench-measured on an E1M-AEN803 (serial
2026W36-0001) / E1M-EVK (hw_rev 2626-r2) with a DesignWare CSI-2 receiver.
Writing `0x0100` = `0x01` (sensor running) and then `0x4800` = `0x25`
(`CLOCK_LANE_GATE|BUS_IDLE|CLOCK_LANE_DISABLE`), `0x4202` = `0x0f`, `0x300d` =
`0x01` — in that order, matching mainline Linux's `ov5647_power_on()` calling
`ov5647_stream_stop()` under the comment "Stream off to coax lanes into LP-11
state" — makes the same register read back `0x00000001` steadily (50/50
samples at 20 ms). **CORRECTION (see `2248-ov5647-root-cause.md`)**: this was
originally attributed to "a hardware fault on the module under test, limiting
it to bit0". That is retracted — the module is healthy. The gap to
`0x00010003` (all three lanes) was the vendored driver's power-on `0x3017`
(LP TX pad drive) left too low, fixed separately by bench run 52's
PLL/MIPI-TX pad-drive divergence. A new `ov5647_lane_park()` helper applies
this sequence,
called at the end of `ov5647_init()` so the sensor is left parked rather than
in bare standby, and again from `ov5647_set_stream(dev, false, ...)` so a
stopped stream still presents LP-11 for the next open.
`ov5647_set_stream(dev, true, ...)` unparks first: `0x4800` = `0x04`
(`BUS_IDLE`), `0x4202` = `0x00`, `0x300d` = `0x00`, then the existing
`0x0100` = `0x01`. `ov5647_set_fmt()` now also drops to standby before its
register writes and re-parks afterwards (it previously ran against a running,
parked sensor), and `ov5647_lane_park()`'s error paths make a best-effort
drop back to standby rather than leaving the sensor streaming mid-sequence.

**Investigated, then reverted — the claimed pixel-rate defect.** The
root-cause note claimed `OV5647_PIXEL_RATE(clk)` = `(clk) * 10 / 3`
(83333333 at 25 MHz XVCLK, a datasheet fps figure) was 2.1x too low against
the sensor's power-on PLL registers, and steered the CSI-2 receiver's D-PHY
frequency-bin lookup to the wrong bin. A follow-up commit changed it to a
PLL-derived 175000000; that change was reverted at the time because nothing
had yet programmed the PLL to match it, and both 83333333- and
175000000-implied lane rates landed in the SAME `{450, 0x16}` D-PHY
frequency bin either way, so the revert did not regress bring-up.
`OV5647_PIXEL_RATE` in `zephyr/drivers/video/ov5647.c` was left
byte-identical to its pre-#2248 form at that point.

**CORRECTION (see `2248-ov5647-root-cause.md`)**: this file previously
concluded from that revert that the pixel-rate claim was "disproven" and
83333333 "approximately correct". That conclusion is retracted. 175000000
was actually right about the sensor's *power-on* link rate, and "it doubled
the programmed VTS" was the correct consequence of a doubled pixel clock,
not evidence the number was wrong. The real defect was that the PLL was
never *written* to match either number — bench run 52 fixes that
separately, and `OV5647_PIXEL_RATE` is corrected there to 87500000 (derived
from the same PLL constants the fix now programs), not the 175000000 this
entry describes as reverted. See `docs/camera-shields.md` for the full,
corrected writeup.
