### Known issue — the RK055HDMIPI4MA0 panel intermittently fails to init, with no recovery (#2199)

On a variable fraction of cold boots of the `e1m_evk_rk055hdmipi4ma0` shield
at the shipped 40 MHz / RGB888-link setting, `hx8394_init()` returns `-EIO`
and the panel device never becomes ready. Zephyr has no re-init path for a display
whose driver failed at `POST_KERNEL`, so an app sees a dead display with no
way to recover short of a power cycle. `examples/aen/aen-dsi-display` now
prints a one-line pointer to this issue when it detects the condition
(`src/main.c`, the `if (!panel_ok)` block).

The signature, in the diagnostic's shipped form --
`zephyr/drivers/mipi_dsi/dsi_dw.c:1363` ("static void dsi_dw_log_fifo_stall(uintptr_t regs, uint32_t pkt_status, uint32_t header)")
-- grep a shipped build for `Failed to write command FIFO`:

```
E: Failed to write command FIFO (header=0x%08x CMD_PKT_STATUS=0x%08x PHY_STATUS=0x%08x).
E: PWR_UP=0x%08x CLKMGR_CFG=0x%08x MODE_CFG=0x%08x CMD_MODE_CFG=0x%08x LPCLK_CTRL=0x%08x
```

The values below, measured on `E1M-AEN803 2026W36-0009`, predate the
`header=` field -- they were captured with the earlier one-line message,
which had no `header=`, `CLKMGR_CFG=` or `LPCLK_CTRL=`:
`CMD_PKT_STATUS=0x00040015`, `PHY_STATUS=0x000015bd`, `PWR_UP=0x00000001`,
`MODE_CFG=0x00000001`, `CMD_MODE_CFG=0x010f7f00`, the escape-clock divider
right, all lanes in Stop -- and no interrupt latched: `CMD_PKT_STATUS` bit
16, `GEN_BUFF_CMD_EMPTY`, is parked at 0, while bits 0, 2 and 18
(`GEN_CMD_EMPTY`, `GEN_PLD_W_EMPTY`, `GEN_BUFF_PLD_EMPTY`) are already set
-- bit 4 (`GEN_PLD_R_EMPTY`) is set too but plays no part in the drain mask
-- so the payload path drained and only the command buffer itself never
did. A rarer stall also hits the first DCS read issued after
`display_write`.

Measured rates, all on the same module. The rate is NOT stable: it varies
between sessions and drifts within one, so treat any single figure as a
sample, not a specification.

- 16 cold boots interleaved one-for-one between the shipped RGB565
  framebuffer and an RGB888 one, both at 40 MHz: 9/16 reached READY (RGB565
  6/8, RGB888 3/8). Failures clustered late in the session -- 0 failures in
  the first four boots, 3 in the last four -- and hit both builds in the same
  stretch, so they track the session, not the framebuffer format.
- Earlier the same day, 15 cold boots of the shipped configuration: 6/15.
- The day before, 10 cold boots of the equivalent RGB888 configuration in a
  logging build: 9/10.

A second, build-independent drift signal from the same session: the board's
pre-load current at 16.0 V (measured before any of our code is loaded) fell
from 0.107 A to 0.089 A over about 25 minutes and then held there. Cause
unknown; recorded because it moves with the failure rate.

A 16-bit-link 57.142857 MHz config tried for 60 Hz is clearly worse, measured
interleaved against 40 MHz in one session, 10 boots each: 5/10 init stalls
versus 1/10, plus read stalls after `display_write` that 40 MHz did not show
at all, 4/10 versus 0/10. The shield overlay's header comment covers why that
config is not shipped.

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
