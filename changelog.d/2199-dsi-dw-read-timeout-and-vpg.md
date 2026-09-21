### Fixed — DCS reads of more than one byte timed out, and the video pattern generator was never armed (#2199)

Three fixes to the DesignWare MIPI-DSI host, found bringing the
RK055HDMIPI4MA0 panel up on the E1M-EVK.

**`MAX_RD_TIME` was sized from the video LP-command window, so the DCS read
timeout moved with the pixel clock.** The driver computed it only from the
DesignWare constraint `[MAX_RD_TIME] * LANEBYTECLK_period < [OUTVACT_LPCMD_TIME]
* 16 * TXCLKESC_period`, which is an *upper* bound and only applies to reads
issued during active video. `MAX_RD_TIME` also has a *lower* bound that formula
ignores — how long the peripheral actually takes to answer — so the timeout was
effectively derived from the line time. Measured on the same panel and driver,
differing only in pixel clock: at RGB888/40 MHz the register read 949 against a
60.0 MHz lane byte clock, a 15.8 µs budget, and `RDDID` (3 bytes) and `RDDST`
(4 bytes) both answered; at RGB565/57.142857 MHz it read 591 against a 57.1 MHz
lane byte clock, a 10.3 µs budget, and both returned `rc=-5` while 1- and
2-byte reads kept working. The value is now floored at what a
read needs — a long response (4-byte header + payload + 2-byte CRC) plus BTA
turnaround and LPDT entry/exit, with margin — and clamped to the register width
`zephyr/drivers/mipi_dsi/dsi_dw.c:387` ("if (outvact > 0) {"). Reads are issued
in command mode, where there is no video LP window to fit inside, so raising it
past the video-derived value costs nothing there.

This is a correctness fix, **not** a fix for the "reads of more than one byte
never answer" behaviour on `E1M-AEN803 2026W36-0009`, and it should not be cited as one.
The bench says so directly: with the floor in place `rdtime` went from `0x24f`
(591) to `0x00000a00` (2560) as intended, and `RDDID` (3 bytes), `RDDST` (4) and
`RDDDB` (5) still returned `rc=-5` while 1- and 2-byte reads kept working and
returned correct data (`RDID1/2/3` = `83 94 0f`, `RDDST2` = `81 73`). The
failures raise `INT_ST1` bit 1, `TO_LP_RX`, and the LP-RX window is not short
either — `LPRX_TO_CNT` is 1000 on a clock of `lanebyteclk / to_clk_div` with
`to_clk_div >= TO_CLK_DIV` (10), i.e. at least ~173 µs at this lane byte clock,
against roughly 10 µs of actual response. The cutoff falls exactly on the
MIPI short-read-response (1-2 bytes) versus long-read-response (3+) boundary,
and the same reads DID work at a different pixel clock, so it is neither a
fundamental protocol gap nor this timeout. Still open.

The same clamp fixes an underflow: `outvact` is signed and has had the
LPDT-entry delay subtracted, so on a short line it can go negative, and the old
expression assigned that straight into a `uint32_t` — producing a *huge*
`MAX_RD_TIME` rather than a small one.

**`VPG_EN` was set at the end of attach, which cannot work.** `dsi_dw_attach()`
deliberately ends in command mode, and the later switch to video in
`dsi_dw_set_mode()` power-cycles the host, clearing the bit. The pattern
generator is now armed in `dsi_dw_video_mode_config()`
`zephyr/drivers/mipi_dsi/dsi_dw.c:812` ("switch (config->dpi.vpg_pattern) {"),
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
`zephyr/drivers/display/display_cdc200.c:489`
("cdc200_shadow_reload_control(DEVICE_MMIO_GET(dev));"). This is belt and
braces, not a fix for a known defect: it was added on the theory that a reload
requested on a stopped CDC does not transfer, and the bench refuted that — the
layer's active registers read back exactly as configured either way.
