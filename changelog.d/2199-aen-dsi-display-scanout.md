### Fixed — the CDC200 -> MIPI-DSI display chain never started its scanout (#2199)

On the Alif E8 display chain, `display_blanking_off()` on the `tes,cdc-2.1`
display now starts the pixel stream. Before this fix, every stage of
`aen-dsi-display` reported READY and `display_write()` returned 0, but the pixels
never left the SRAM0 framebuffer. Three things kept the scanout off:

- `dsi_dw_set_mode()` had no callers, so the DSI host stayed in the command mode
  that `dsi_dw_attach()` leaves it in;
- `cdc200_set_enable()` had no callers, so `CDC_EN` was never set;
- `cdc200_blanking_off()` / `cdc200_blanking_on()` returned `-ENOTSUP`.

`cdc200_blanking_off()` now switches the DesignWare DSI host whose `cdc-if` names
this CDC to video mode, then sets `CDC_EN`. `cdc200_blanking_on()` does the
reverse, in reverse order, so the DSI host is reset only after the CDC has been
told to stop feeding it. Whether `CDC_EN=0` stops at the end of the frame is not
verified, so the last frame may still be cut. The panel's init sequence still runs in command mode first:
in DW video mode, commands only leave during DPI blanking, and there is none
before `CDC_EN`. With no DSI host, blanking is just `CDC_EN`, for a parallel-RGB
panel.

Two `dsi_dw.c` defects that only video mode exposes are fixed too.
`dsi_dw_set_mode()` now records `curr_mode` when switching back to command mode;
without that, a blanking on/off cycle never re-entered video. `dsi_dw_msg_config()`
no longer clears `PHY_TXREQUESTCLKHS` for an LP command while in video mode,
which would have stopped the HS clock under the running scanout.

`dsi_dw_attach()` also stops trusting `mdev->timings`. It used the `cdc-if`
controller's timings only when the panel driver passed `hactive == 0`, but the
upstream `himax,hx8394` driver leaves `mdev->timings` uninitialized on its stack.
So the D-PHY lane rate, the DPI line/frame registers and the LP-command windows
were computed from stack contents: the same code gave different
`DSI_DPI_LP_CMD_TIM` / `DSI_PHY_TMR_RD_CFG` values on two builds. The host packs
exactly the DPI stream the CDC generates, so it now always programs the `cdc-if`
timings.

The DSI host now takes the DPI pixel clock from the `cdc-if` controller's
`clock-frequency` instead of assuming 60 Hz. The example ran its CDC at 400/6 =
66.67 MHz into a host timed for 59.98 MHz, so the DPI payload FIFO overflowed on
every line (`INT_ST1` `DPI_PLD_WR_ERR`). The `cdc200` node now declares the rate
the CDC really runs, 400/10 = 40 MHz (`clock-frequency = <40000000>`, in the
`e1m_evk_rk055hdmipi4ma0` shield; `90d450387` moved it here from 400/7 =
57.142857 MHz), and the SoC glue derives the CDC divider from that same
property, so the two can no longer disagree. 66.67 MHz would need about
539 Mbps per lane, above the host's default 500 Mbps `panel-max-lane-bandwidth`.

`dw_setup_timeout()` no longer truncates the non-burst HS-TX timeout. A 720x1280
frame (about 1.1M lane-byte clocks) does not fit the 16-bit `HSTX_TO_CNT` at the
fixed timeout-clock divider of 10, and the truncated count was about 8 ms, so
`TO_HS_TX` fired inside every ~17 ms frame. The divider now widens until the
count fits.

The host's DSI mode now starts as command mode, which is where it comes out of
reset, and returning to command mode drops the HS clock request that video mode
set.

`dsi_dw_set_mode()` now returns `-ENODEV` until `dsi_dw_attach()` has succeeded,
so `display_blanking_off()` can no longer report success on a host that was
never configured. `cdc200_set_enable()` is removed: it toggled `CDC_EN` without
the DSI mode and had no callers.

The display chain no longer requests a peripheral ACK at the end of every frame
(`frame-ack-en`; neither the SoC `mipi_dsi` node nor the shield sets it). The upstream RK055 setup does not use it, and a frame whose ACK
never comes stalls the host in LP receive long enough to overflow the DPI
payload FIFO.
