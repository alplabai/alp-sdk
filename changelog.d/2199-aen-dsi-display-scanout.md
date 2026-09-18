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
reverse, in reverse order, so the DSI mode switch (which resets the host) never
cuts a live DPI frame. The panel's init sequence still runs in command mode first:
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
