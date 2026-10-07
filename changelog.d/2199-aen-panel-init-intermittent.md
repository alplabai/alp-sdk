### Known behaviour — the RK055HDMIPI4MA0 panel's first init stalls on a variable fraction of boots; the shipped retry recovers it (#2199)

On a variable fraction of boots of the `e1m_evk_rk055hdmipi4ma0` shield at the shipped 40 MHz / RGB888-link setting, the first `hx8394_init()` returns `-EIO` because one DCS write stalls in the DSI host's command FIFO. The SDK retries the init before `main()` (`src/zephyr/panel_init_retry.c`, up to five retries, recorded in `2199-panel-init-retry.md`), so an app gets a working panel with no code of its own; `examples/aen/aen-dsi-display` prints a one-line pointer to this issue if all attempts fail.

The signature, in the diagnostic's shipped form (`zephyr/drivers/mipi_dsi/dsi_dw.c`, `dsi_dw_log_fifo_stall()`) -- grep a build for `Failed to write command FIFO`:

```
E: Failed to write command FIFO (header=0x%08x CMD_PKT_STATUS=0x%08x PHY_STATUS=0x%08x).
E: PWR_UP=0x%08x CLKMGR_CFG=0x%08x MODE_CFG=0x%08x CMD_MODE_CFG=0x%08x LPCLK_CTRL=0x%08x
```

Measured on `E1M-AEN803 2026W36-0009`, with the earlier one-line message that had no `header=`, `CLKMGR_CFG=` or `LPCLK_CTRL=`: `CMD_PKT_STATUS=0x00040015`, `PHY_STATUS=0x000015bd`, `PWR_UP=0x00000001`, `MODE_CFG=0x00000001`, `CMD_MODE_CFG=0x010f7f00`, all lanes in Stop and no interrupt latched (`GEN_BUFF_CMD_EMPTY`, bit 16, parked at 0 while bits 0, 2 and 18 are set): the payload path drained and only the command buffer did not. A rarer stall also hits the first DCS read after `display_write`.

Rates without the retry, same module; the rate is not stable (it varies between sessions and drifts within one), so each figure is a sample: 6 of 15 cold boots reached READY, then 9 of 16 in a later session (RGB565 framebuffer 6/8, RGB888 3/8, failures clustered late in the session and hitting both builds). A 16-bit-link 57.142857 MHz config tried for 60 Hz is worse, 5/10 init stalls against 1/10 at 40 MHz in the same session, and adds read stalls; it is not shipped (the shield overlay's header comment has the reason).

With the retry, on `2026W36-0002`: `RESULT PASS` 5 of 5 RAM-runs over a resident app that had already initialised the panel, the worst needing attempt 4 (attempts 1-3 returned `-5`). That is the residual: no failure in 5 runs; the rate is not characterised beyond that.
