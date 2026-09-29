### Fixed — `gpio-gd32-bridge` probe retries `GET_VERSION` and no longer races its own workqueue cancellation on unbind/rebind (#2297)

Bench-observed on E1M-V2M103, 2026-09-29, in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0005-gpio-add-gd32-bridge-expander-driver.patch`
(`drivers/gpio/gpio-gd32-bridge.c`). Two issues, both in
`gd32_bridge_gpio_probe()`.

(1) The probe-time `GET_VERSION` handshake made exactly one attempt: at a
cold boot the GD32 is still coming up at ~1.95 s in, so this consistently
missed — observed as `-EBADMSG` (-74, a torn reply) and `-EBUSY` (-16,
arbitration lost on a bus the GD32 hasn't released yet) — while a
`GET_VERSION` issued a few seconds later on the same bridge succeeded
cleanly (reply `00 00 0e 00 cf a7`). The "registering gpiochip anyway"
best-effort design (Refs #2297) is correct — a `-EPROBE_DEFER` here would
hold the whole DSI/DU pipeline hostage to the bridge's liveness via the
Display-1 panel reset line — but paying for only one attempt when the
bridge is this close to answering just pushed the wait onto
`gd32_bridge_resolve_work()`'s 1 Hz poller instead. Fixed with a bounded
retry: up to 3 attempts, 10-20 ms apart, before falling back to the
existing "bridge not answering" path unchanged.

(2) `echo 8-0070 > /sys/bus/i2c/drivers/gpio-gd32-bridge/unbind` then bind
printed a WARN stack trace at `gd32_bridge_gpio_probe+0x2cc` on the
re-probe. Root cause: `devm_gpiochip_add_data()` was registered *last* in
probe, after the two `devm_add_action_or_reset()` calls that cancel
`replay_work`/`resolve_work`. `devm` teardown runs LIFO, so on unbind the
gpiochip was removed *first*, while the driver's own delayed work could
still be queued or actively running an I2C transfer against the chip/state
being torn down under it — a race against this driver's own workqueue, not
against the other consumers (`wlan-pwrseq`, `hci_bcm`) of these lines.
Fixed by registering `devm_gpiochip_add_data()` before the two cancel
actions, so LIFO teardown now cancels and confirms both delayed_work items
finished (`cancel_delayed_work_sync()`) *before* the gpiochip they touch is
removed.

Compile-checked out-of-tree against the real E1M-V2M103 kernel source and
a configured `kernel-build-artifacts` tree with the SDK cross-toolchain;
bench-pending (no bench access from this change).

On a reply CRC mismatch the driver now logs the raw reply bytes (`cmd 0x01: reply CRC mismatch: ...`, rate-limited), so the next cold-boot `-EBADMSG` shows which bytes were torn instead of only the errno (alplabai/gd32-bridge-firmware#295).
