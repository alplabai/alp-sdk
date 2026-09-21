### Added — `e1m_evk_rpi_csi` shield wires the E1M-EVK Raspberry Pi camera connector to the AEN CSI-2 pipeline; RAW10 capture no longer overruns its buffer (#226)

**New board-side shield.** `zephyr/boards/shields/e1m_evk_rpi_csi` is the board
half of Zephyr's Raspberry Pi camera-shield contract for an E1M-AEN SoM on the
E1M-EVK. It provides `csi_interface` (`&csi`), `csi_ep_in` (CSI port@0),
`csi_i2c` (SoC `i2c1`, SCL `P3_7` function B / SDA `P7_2` function C) and
`csi_capture_port` (`&cam`), builds the csi-to-cam endpoint graph, hogs E1M
`IO2` (`P12_5`, the PI3WVR626 SEL) low so J5 on mux input A reaches the SoC,
and sets `alp-camera0 = &cam`. Those labels map onto SoC nodes and pins, so
they live in per-target overlays under the shield's `boards/` directory: the
`alp_e1m_aen801_m55_he` and `alp_e1m_aen803_m55_he` overlays both include
`boards/e1m_aen.dtsi`, and the main overlay keeps only the board-agnostic
alias. Pair it with a sensor shield:
`-DSHIELD="e1m_evk_rpi_csi raspberry_pi_camera_module_2"` compiles and links
with the upstream IMX219 driver for `alp_e1m_aen801_m55_he` and
`alp_e1m_aen803_m55_he`. Not yet run on hardware.

**The camera now uses the dedicated CSI-2 receive D-PHY.** The SoC default
`phy-if = <&dphy 1>, <&dphy 0>` selects D-PHY id 1, the DSI transmit D-PHY
running in receive mode. The shield overrides it to `<&dphy 0>` with the sensor
on port@0, the two being coupled in `video_csi_dw.c`.

**The SoC `dphy` node carries its four real clock gates.** `ensemble_e8_peripherals.dtsi`
now lists `ALIF_MIPI_PLLREF_CLK`, `ALIF_MIPI_BYPASS_CLK`, `ALIF_MIPI_TXDPHY_CLK`
and the new `ALIF_MIPI_RXDPHY_CLK` (`MIPI_CKEN` bits 8 / 12 / 0 / 4) instead of
the frequency-only `ALIF_CSI_DPHY_CLK` placeholder, and the `aen-dsi-display`
overlays no longer override them. Every build that enables `dphy` gets all four
gates, so the camera shield stacked on a DSI app keeps its receive D-PHY gate.

**Real camera clock IDs.** `ALIF_CPI_CLK` (`PERIPH_CLK_ENA` bit 0) and
`ALIF_CSI_CLK` (`PERIPH_CLK_ENA` bit 24) replace the frequency-only dummy. The
`csi` node's `pix_clk` is now `ALIF_CSI_PIX_SYST_ACLK` (`CSI_PIXCLK_CTRL`
`CLK_ENA` bit 0, `CLK_SEL` bit 4 = 0, 400 MHz SYST_ACLK), and the `cam` node
gains a `pix_clk`, the new `ALIF_CAM_PIX_SYST_ACLK` (`CAMERA_PIXCLK_CTRL`, same
fields). In CSI mode the CPI runs that clock at the rate the CSI bridge
programmed. Both pixel clocks are now enabled only after their divisor is set,
the order the Alif DFP uses; the CSI bridge had enabled its pixel clock at init
on the reset divisor.

**The camera pixel-clock dividers are programmable.** The Alif clock-control
patch (`zephyr/patches/zephyr/0001-clock_control_alif-master-source-expmst-i2s-setrate.patch`)
now gives `.set_rate` / `.get_rate` the `CAMERA_PIXCLK_CTRL` / `CSI_PIXCLK_CTRL`
divisor (bits [24:16], 2..0x1FF). `set_rate` picks the slowest reachable rate
that is still at least the request and returns `-ERANGE` above source / 2.
`set_rate` also programs the `CLK_SEL` the clock ID selects, as
`clock_control_on()` does, so it does not depend on the `CLK_SEL` boot firmware
left and can run before the clock is enabled. `CLK_SEL` = 1 is refused because
its rate is not yet settled. Before this, the CSI bridge's
`clock_control_set_rate(pix_clk)` failed with `-ENOTSUP`, so `video_set_format()`
could never succeed on hardware. A workspace that applied the previous version
of this patch now reports it as drifted: re-apply it with
`west patch --dst-module zephyr clean` then `west patch --dst-module zephyr apply`.
The clean step discards local edits in the zephyr checkout.

**The CSI pixel clock gives up its margin before it gives up.**
`csi2_dw_validate_data()` asks for 1.2 x the pixel rate. When that exceeds the
200 MHz maximum but the bare pixel rate fits, it now runs at the maximum and
logs a warning instead of failing. It fails with `-ERANGE` only when even the
bare rate does not fit, and the error names a wider format or a lower link
frequency as the fix. A 2-lane IMX219 at 456 MHz therefore streams RAW10
(182.4 Mpixel/s, run at 200 MHz); RAW8 (228 Mpixel/s) is refused.

**RAW10 Bayer is accepted.** `VIDEO_PIX_FMT_SBGGR10P`, `SGBRG10P`, `SGRBG10P`
and `SRGGB10P` now map to CSI-2 data type RAW10 in both `video_csi_dw.c` and
`video_alif.c`. Before, only `Y10P` did, so a RAW10 Bayer sensor such as the
IMX219 was rejected. `video_alif.h` aliases its old `BGGR10` names to the
upstream `SBGGR10` symbols instead of redefining the FOURCCs.

**The CPI reports the memory layout it writes.** In CSI mode the CPI stores
RAW10/12/14 as one 16-bit sample per pixel, but it kept whatever pitch the
caller passed: 0 from the portable camera backend, or width x 10 / 8 from a
packed-RAW10 sensor. The enqueue size guard then accepted a buffer that the
DMA overran by 60 %. `alif_cam_set_fmt()` now sets the pitch to width x the
data-mode container (1, 2 or 4 bytes) and returns the unpacked FOURCC
(`SBGGR10`, `Y10`, ...), while the sensor is still asked for the packed format.
A parallel (CPI or LPCPI) camera gets the same rule from its DT `data-mode`, so
a pitch of 0 no longer lets the guard pass any buffer.

**The CSI-2 lane rate comes from the sensor.** `video_csi_dw.c` now reads it
with `video_get_csi_link_freq()` (the sensor's `VIDEO_CID_LINK_FREQ`, 456 MHz
for the IMX219) and uses the DT `link-frequencies` / `rx-ddr-clk1` value only as
a fallback. Upstream sensor shields carry no `link-frequencies`, so the D-PHY
had been set up for the 400 MHz default. `video_set_format()` now redoes the
full setup on every call: a second call at a new resolution with the same CSI-2
data type used to return early and keep the old line timing, lane rate and
pixel clock.

**Buffer starvation now pauses capture instead of stopping it for good.**
`video_alif.c` used to stop the endpoint and never restart it once the
application held its one in-flight buffer for more than a frame period; it now
marks the stream starved and the next `video_enqueue()` reprograms
`CAM_FRAME_ADDR` and resumes capture (bench-proven 2026-09-21 on an
E1M-AEN803 on the E1M-EVK, OV9281 on J5: 60-frame wall-clock capture
bursts in all three OV9281 modes each landed at their configured frame
rate, which a stream that stalled on starvation could not have done).
