### Fixed — the Linux `gpio-gd32-bridge` driver now replays requested GPIO output state after the GD32 comes up late or resets (#2297)

Bench-proven on E1M-V2M103, 2026-09-26. Two related bugs in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0005-gpio-add-gd32-bridge-expander-driver.patch`
(`drivers/gpio/gpio-gd32-bridge.c`): (1) `gd32_bridge_gpio_set()` only
latched `output_mask`/`output_vals` **after** a successful I2C
`GPIO_WRITE`, so a write attempted while the bridge was not yet
answering was silently forgotten forever — seen at probe as `GPIO write
line 5 failed`, the Display-1 panel `reset-gpios` hog, since the
gpiochip registers unconditionally even while the bridge is still down;
(2) nothing re-applied host-requested state after the GD32 came up late
or after any GD32 reset (WDT, an OTA A/B swap), both of which reset
every pad to its boot default — including the Wi-Fi/BT REG_ON lines
18/19, which this issue originally tracked.

Fixed by (1) latching `output_mask`/`output_vals` under a new
`state_lock` **before** attempting the write, so a failed transfer no
longer loses the requested state, and (2) a `delayed_work` started at
`probe()` that re-issues one idempotent `CMD_GPIO_WRITE(output_mask,
output_vals)` roughly once a second whenever `output_mask != 0`.
Re-asserting an already-correct level is glitch-free, so the replay
needs no reset detection — it just needs to run often enough that a
freshly-up bridge is re-promoted within about one period. Deliberately
does **not** use `CMD_RESET_REASON`: that opcode is clear-on-read on
the firmware side, so polling it would itself destroy the very signal
a second reader needs. The replay masks lines 18/19 back out unless
`.request()`'s existing protocol-version gate (`gd32_bridge_
resolve_wifi_bt()`) has already confirmed the bridge understands them.
Costs one small I2C frame/s on BRD_I2C (`i2c8`) while any line is held
as output.

As a consequence, `direction_output()` now always reports success
instead of propagating the transfer error: the requested state is
latched and will be replayed regardless, and this callback also runs
for OF `gpio-hog` lines during the gpiochip's own registration, where a
nonzero return would fail `devm_gpiochip_add_data()` outright — losing
the entire 20-line chip instead of just the one hog.

See also: `docs/gd32-bridge-protocol.md` §3.1 (host-behaviour note,
updated in the same change).
