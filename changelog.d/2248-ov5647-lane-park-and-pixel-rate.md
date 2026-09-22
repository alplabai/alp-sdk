### Fixed — OV5647: park the CSI-2 lanes at LP-11; the claimed pixel-rate defect was disproven (#2248)

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
samples at 20 ms; the module under test has a hardware fault on DATA_1/CLK,
limiting it to bit0, which does not affect this fix — a healthy module should
reach `0x00010003`). A new `ov5647_lane_park()` helper applies this sequence,
called at the end of `ov5647_init()` so the sensor is left parked rather than
in bare standby, and again from `ov5647_set_stream(dev, false, ...)` so a
stopped stream still presents LP-11 for the next open.
`ov5647_set_stream(dev, true, ...)` unparks first: `0x4800` = `0x04`
(`BUS_IDLE`), `0x4202` = `0x00`, `0x300d` = `0x00`, then the existing
`0x0100` = `0x01`. `ov5647_set_fmt()` now also drops to standby before its
register writes and re-parks afterwards (it previously ran against a running,
parked sensor), and `ov5647_lane_park()`'s error paths make a best-effort
drop back to standby rather than leaving the sensor streaming mid-sequence.

**Disproven, then reverted — the claimed pixel-rate defect.** The root-cause
note claimed `OV5647_PIXEL_RATE(clk)` = `(clk) * 10 / 3` (83333333 at 25 MHz
XVCLK, a datasheet fps figure) was 2.1x too low against the sensor's power-on
PLL registers, and steered the CSI-2 receiver's D-PHY frequency-bin lookup to
the wrong bin. A follow-up commit changed it to a PLL-derived 175000000. Both
claims were wrong: mainline Linux `drivers/media/i2c/ov5647.c` declares
`pixel_rate = 87500000` with `link_freq = 218750000` for this same 2592x1944
mode — `87500000 * 10 / (2 lanes * 2)` = `218750000` exactly, so the real
lane rate is 437.5 Mbps, not the 875 Mbps the PLL-derived attempt assumed (the
`* lanes` factor was the error). 83333333 is within about 5% of mainline's
figure, and both 416 Mbps (from 83333333) and 437.5 Mbps (mainline) land in
the SAME `{450, 0x16}` row of `frequency_range[]` in
`zephyr/drivers/mipi_dphy/dphy_dw.c` — the claimed wrong bin never existed.
Worse, `OV5647_PIXEL_RATE` also feeds `ov5647_frmrate_to_vts()`, which divides
it to produce the programmed `TIMING_VTS`: doubling the macro to 175000000
while leaving the PLL untouched halved the real frame rate and made
`ov5647_enum_frmival()` advertise an unreachable 30 fps at full resolution.
The 175000000 change is reverted; `OV5647_PIXEL_RATE` in
`zephyr/drivers/video/ov5647.c` is byte-identical to its pre-#2248 form, and a
comment there records the mainline comparison and the revert for the next
reader. See `docs/camera-shields.md` for the full writeup.
