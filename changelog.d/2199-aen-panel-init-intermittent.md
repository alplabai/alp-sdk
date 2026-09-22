### Known issue — the RK055HDMIPI4MA0 panel intermittently fails to init, with no recovery (#2199)

On roughly 1 in 8-10 cold boots of the `e1m_evk_rk055hdmipi4ma0` shield at the
shipped 40 MHz / RGB888-link setting, `hx8394_init()` returns `-EIO` and the
panel device never becomes ready. Zephyr has no re-init path for a display
whose driver failed at `POST_KERNEL`, so an app sees a dead display with no
way to recover short of a power cycle. `examples/aen/aen-dsi-display` now
prints a one-line pointer to this issue when it detects the condition
(`src/main.c`, the `if (!panel_ok)` block).

The signature, measured on `E1M-AEN803 2026W36-0009`:

```
E: Failed to write command FIFO (CMD_PKT_STATUS=0x00040015 PHY_STATUS=0x000015bd)
```

with the host correctly configured at the moment of the stall --
`PWR_UP=0x00000001`, `MODE_CFG=0x00000001`, `CMD_MODE_CFG=0x010f7f00`, the
escape-clock divider right, all lanes in Stop -- and no interrupt latched
(`CMD_PKT_STATUS` bit 2, `GEN_BUFF_CMD_EMPTY`, parked at 0 with nothing else
wrong to explain why). A rarer stall also hits the first DCS read issued
after `display_write`.

Measured rate, 10 cold boots per configuration: 1/10 at the shipped 40 MHz
RGB888-link setting; 5/10 at a genuine 16-bit-link 57.142857 MHz config tried
for 60 Hz (the shield overlay's own header comment covers why that config is
not shipped); the 16-bit-link config also added read stalls the 40 MHz
config did not show, 4/10 versus 0/10.

Ruled out, none of it the cause:
- the escape clock divider (12 MHz versus 15 MHz -- `MAX_ESC_CLK` change in
  `changelog.d/2199-dsi-dw-read-timeout-and-vpg.md`);
- a 300 ms panel-power settle inserted before the panel driver's reset pulse;
- reordering `dsi_dw_transfer_locked()` to power the host up BEFORE
  `dsi_dw_setup_lp_cmd()`/`dsi_dw_msg_config()`, paired with a bounded
  stop-state settle poll in `dsi_dw_pwr_up_once()`. Both were tried and
  measured across 20 cold boots (10 per pixel-clock config): the settle poll
  read 0 us on 20/20 boots (dead code -- stop state was never the
  bottleneck), and the stall rate was unchanged from the pre-reorder
  baseline. The reorder was also reviewed and found harmful in principle --
  `dsi_dw_attach_locked()` leaves `LPCLK_CTRL` with `PHY_TXREQUESTCLKHS` set,
  so powering up before `dsi_dw_msg_config()` starts the HS clock and then
  drops it, adding an LP->HS->LP excursion immediately before the first LP
  command that Linux's `dw-mipi-dsi` does not have (it programs these
  registers with the core in reset and waits for clock-lane stop before
  power-up). Both changes were reverted.

No retry or recovery is implemented. A future fix needs either a
Zephyr-side re-init path for a `POST_KERNEL` device that failed, or an
app-level retry loop around `hx8394_init()`'s effects (which Zephyr's driver
model does not expose a way to re-invoke without a reboot).
