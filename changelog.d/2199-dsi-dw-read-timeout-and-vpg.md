### Fixed — DCS reads of more than one byte timed out, and the video pattern generator was never armed (#2199)

Three fixes to the DesignWare MIPI-DSI host, found bringing the
RK055HDMIPI4MA0 panel up on the E1M-EVK.

**`MAX_RD_TIME` was sized from the video LP-command window, so the DCS read
timeout moved with the pixel clock.** The driver computed it only from the
DesignWare constraint `[MAX_RD_TIME] * LANEBYTECLK_period < [OUTVACT_LPCMD_TIME]
* 16 * TXCLKESC_period`, which is an *upper* bound and only applies to reads
issued during active video. `MAX_RD_TIME` also has a *lower* bound that formula
ignores — how long the peripheral actually takes to answer — so the timeout was
effectively derived from the line time, not the read. On the same panel and
driver, differing only in pixel clock, the computed register value moves with
it: RGB888/40 MHz gives a 60.0 MHz lane byte clock and `rdtime` = 949 (a
15.8 µs budget); RGB565/57.142857 MHz gives a 57.1 MHz lane byte clock and
`rdtime` = 591 (a 10.3 µs budget). The value is now floored at what a
read needs — a long response (4-byte header + payload + 2-byte CRC) plus BTA
turnaround and LPDT entry/exit, with margin — and clamped to the register width
`zephyr/drivers/mipi_dsi/dsi_dw.c:479` ("if (outvact > 0) {"). Reads are issued
in command mode, where there is no video LP window to fit inside, so raising it
past the video-derived value costs nothing there.

This is a correctness fix on its own terms, and it is not what closed the
"reads of more than one byte never answer" behaviour seen on
`E1M-AEN803 2026W36-0009` — that symptom was never a MAX_RD_TIME problem.
With the floor in place, `rdtime` went from `0x24f` (591) to `0x00000a00`
(2560) as intended, but `RDDID` (3 bytes), `RDDST` (4) and `RDDDB` (5) still
returned `rc=-5` (`INT_ST1` bit 1, `TO_LP_RX`) while 1- and 2-byte reads kept
working — across four bench runs, 100% correlated with the pixel clock
(failing at RGB565/57.142857 MHz, answering at RGB888/40 MHz). That
correlation was confounded: a marginal FFC contact on the panel flex was
corrupting the DCS read path for the whole campaign, and every one of those
runs was measured through it. Before a connector reseat, every DCS read
returned `rc=-5` with `int1=0x00000002` (`TO_LP_RX`) and `SETEXTC` did not
land at all. After the reseat, on the same build, `SETEXTC` gates as expected,
and `RDDID` returns `rc=3 data=83 94 0f`, `RDDPM=1c`, `RDDST=80 73 04 00` —
3-, 4- and 5-byte reads now answer correctly at BOTH pixel clocks. The
short-versus-long-read-response boundary the failures had fallen on was a
property of what a bad LP-RX turnaround corrupts, not a MIPI protocol limit or
this timeout.

What actually distinguishes the two pixel clocks now is not reads at all. A
controlled A/B on the same board and the same (good) contact state, 100 ms of
scanout each: RGB888/40 MHz gives `int0=0x00000000 int1=0x00000000
dpi-wr-err=0 dpi-under=0 pixclk-ctrl=0x000a0001`, with pixels on glass;
RGB565/57.142857 MHz gives `int0=0x00000000 int1=0x00000080 dpi-wr-err=1
dpi-under=0 pixclk-ctrl=0x00070001`, still set after 5 s of video, with glass
dark. `INT_ST1` bit 7 is `DSI_INT_1_DPI_PLD_WR_ERR`
`zephyr/drivers/mipi_dsi/dsi_dw.h:381` ("#define  DSI_INT_1_DPI_PLD_WR_ERR		BIT(7)"):
the DPI payload FIFO overflows continuously at the higher pixel clock. That
overflow, not a DCS read timeout, is why the shield stays at 40 MHz. It is a
link-bandwidth limit, not a host defect: the A/B changed only the CDC layer to
RGB565, and the layer format never reaches the link, which runs at the panel's
`pixel-format` (RGB888). 57.142857 MHz at 24 bpp needs about 690 Mbps per
lane, the host clamps to the 500 Mbps `panel-max-lane-bandwidth`, and the line
then comes out at 762 * 62.5 / 57.142857 = 833 lane-byte clocks -- the
`hline` the host programmed -- against about 1086 of payload. A 16-bit link
(panel `pixel-format = <MIPI_DSI_PIXFMT_RGB565>`) would need about 462 Mbps
per lane; it has not been tried on a powered panel.

The same clamp fixes an underflow: `outvact` is signed and has had the
LPDT-entry delay subtracted, so on a short line it can go negative, and the old
expression assigned that straight into a `uint32_t` — producing a *huge*
`MAX_RD_TIME` rather than a small one.

**`VPG_EN` was set at the end of attach, which cannot work.** `dsi_dw_attach()`
deliberately ends in command mode, and the later switch to video in
`dsi_dw_set_mode()` power-cycles the host, clearing the bit. The pattern
generator is now armed in `dsi_dw_video_mode_config()`
`zephyr/drivers/mipi_dsi/dsi_dw.c:1004` ("switch (config->dpi.vpg_pattern) {"),
where it survives. Any VPG result taken before this change is void.

**A board can now declare the video mode and EoTp its panel needs.** A panel
driver is supposed to do this, but upstream ones frequently do not: Zephyr's
`himax,hx8394` sets `mdev.mode_flags = MIPI_DSI_MODE_VIDEO` and nothing else, so
burst and EoTp could never reach this host whatever the panel wanted. The new
`dpi-video-mode` and `autoinsert-eotp` properties
`zephyr/dts/bindings/mipi-dsi/snps,designware-dsi.yaml:74` ("dpi-video-mode:")
are ORed into the flags the panel driver supplies, because the board is what
knows which panel is fitted. CRC and ECC reception are now driven by their
devicetree properties instead of being forced on; `BTA_EN` stays forced, since
it gates bus turnaround for DCS reads and is unrelated to `frame-ack-en`.

`MAX_ESC_CLK` drops from the 20 MHz D-PHY ceiling to 15 MHz. The divider is
integer, so a lane byte clock just above a multiple lands the escape clock
within a few percent of the spec maximum, leaving a peripheral's LP receiver no
margin; Linux's `dw-mipi-dsi` sizes the same divider against 15 MHz, and
matching it costs only LP command time.

The CDC200 also reloads its shadow registers once the controller is running
`zephyr/drivers/display/display_cdc200.c:507`
("cdc200_shadow_reload_control(DEVICE_MMIO_GET(dev));"). This is belt and
braces, not a fix for a known defect: it was added on the theory that a reload
requested on a stopped CDC does not transfer, and the bench refuted that — the
layer's active registers read back exactly as configured either way.

**Both command-FIFO timeout sites now log `DSI_CMD_PKT_STATUS` and
`DSI_PHY_STATUS` at the moment of the stall**, since a FIFO that never drains
latches no interrupt and the bare "Failed to write command FIFO." message
said nothing about why
`zephyr/drivers/mipi_dsi/dsi_dw.c:1396`
("static void dsi_dw_log_fifo_stall(uintptr_t regs, uint32_t pkt_status)").
`PHY_LOCK`, `PHY_DIRECTION`, and the clock/lane-0/lane-1 stop-state bits each
narrow the stall to a different signature — a lost PLL, a stuck bus
turnaround, or a lane that never returned to LP-11 — found while chasing
intermittent HX8394 init failures on `E1M-AEN803 2026W36-0009` (#2199). The
new log distinguishes those signatures; it does not say which one is the
root cause on this board.

**A separate intermittent first-command stall, seen on about 1 in 6 cold
boots of `E1M-AEN803 2026W36-0009`, gets an ordering fix plus more
instrumentation; it is not yet bench-verified.** The stall parked the first
DSI command in the packet handler (`GEN_BUFF_CMD_EMPTY` stuck low,
`CMD_PKT_STATUS=0x00040015 PHY_STATUS=0x000015bd`: PHY locked, all lanes in
Stop, no interrupt), and a second, rarer stall hit the first DCS read after
`display_write` (`CMD_PKT_STATUS=0x00040055` `GEN_RD_CMD_BUSY`,
`PHY_STATUS=0x00001529`, no lane in Stop). `dsi_dw_transfer_locked()` now
powers the host up before `dsi_dw_setup_lp_cmd()`/`dsi_dw_msg_config()` touch
`DSI_CMD_MODE_CFG`/`DSI_LPCLK_CTRL`, not after
`zephyr/drivers/mipi_dsi/dsi_dw.c:1060` ("dsi_dw_pwr_up_once(dev);") --
Linux `dw-mipi-dsi` parity, since the old order let those two calls run
while `SHUTDOWNZ` was still 0. `dsi_dw_pwr_up_once()`
`zephyr/drivers/mipi_dsi/dsi_dw.c:104`
("static void dsi_dw_pwr_up_once(const struct device *dev)") now polls
`DSI_PHY_STATUS` for the stop-state bits after power-up, bounded at 10 ms,
and logs how long the poll actually waited -- the ordering change is the
primary fix, and this log is instrumentation to show whether stop state was
ever the bottleneck, not a claimed cure on its own. `dphy_dw_master_setup()`
also now writes `PHY_STOP_WAIT_TIME(0x20)` alongside `N_LANES` in
`DSI_PHY_IF_CFG`
`zephyr/drivers/mipi_dphy/dphy_dw.c:387`
("reg_write_part(dsi_regs + DSI_PHY_IF_CFG, DSI_PHY_IF_CFG_PHY_STOP_WAIT_TIME_VAL,")
-- Linux parity again; the field was left at its reset value of 0, and the
vendored local macro for it carried the wrong bit shift (it collided with
`N_LANES`), fixed alongside wiring the field up. `dsi_dw_log_fifo_stall()`
`zephyr/drivers/mipi_dsi/dsi_dw.c:1396`
("static void dsi_dw_log_fifo_stall(uintptr_t regs, uint32_t pkt_status)")
now also logs `DSI_PWR_UP`, `DSI_CLKMGR_CFG`, `DSI_MODE_CFG`,
`DSI_CMD_MODE_CFG` and `DSI_LPCLK_CTRL`, to tell a dropped config write
(host still in reset) from a mode mismatch (a read issued in video mode)
the next time either stall recurs. None of this has been run on hardware
yet; bench verification against the ~1-in-6 failure rate is pending.
