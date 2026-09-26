### Fixed — the Linux `gpio-gd32-bridge` driver now replays requested GPIO output state after the GD32 comes up late or resets (#2297)

Bug observed and fix bench-verified on E1M-V2M103 (GD32 fw 0.2.13), 2026-09-26.
Two cases were checked. At cold boot, probe finds the bridge down and the
panel-reset write fails. The replay then logs `bridge reachable, 1 line(s)
re-applied` at ~5 s, with no driver rebind. With GD32_NRST held for 2 s
mid-run, the log shows `output state replay failed (-6)` and then
`re-applied`, and `GPIO_READ` confirms the pad level. Two related bugs in
`meta-alp-sdk/recipes-kernel/linux/linux-renesas/0005-gpio-add-gd32-bridge-expander-driver.patch`
(`drivers/gpio/gpio-gd32-bridge.c`): (1) `gd32_bridge_gpio_set()` only
latched `output_mask`/`output_vals` **after** a successful I2C
`GPIO_WRITE`, so a write attempted while the bridge was not yet
answering was silently forgotten forever — seen at probe as `GPIO write
line 5 failed` against the Display-1 panel `reset-gpios` consumer,
since the gpiochip registers unconditionally even while the bridge is
still down; (2) nothing re-applied host-requested state after the GD32
came up late or after any GD32 reset (WDT, an OTA A/B swap), both of
which reset every pad to its boot default — including the Wi-Fi/BT
REG_ON lines 18/19, which this issue originally tracked.

Fixed by (1) latching `output_mask`/`output_vals` under a new
`state_lock` and holding that lock across the *entire* transfer (not
just the latch) in both the write path and the replay path, so a
concurrent replay can never read a stale snapshot and land its write
after a newer one — driving a line back down (e.g. `wl-reg-on` low
again mid `mmc-pwrseq`, or the panel reset line) — and (2) a
`delayed_work` that re-issues one idempotent `CMD_GPIO_WRITE(output_mask,
output_vals)` roughly once a second whenever `output_mask != 0`, queued
on `system_freezable_power_efficient_wq` (not the plain system
workqueue, which would keep running — and touching I2C — across
suspend). Re-asserting an already-correct level is glitch-free, so the
replay needs no reset detection — it just needs to run often enough
that a freshly-up bridge is re-promoted within about one period.
Deliberately does **not** use `CMD_RESET_REASON`: that opcode is
clear-on-read on the firmware side, so polling it would itself destroy
the very signal a second reader needs. The replay masks lines 18/19
back out unless `.request()`'s existing protocol-version gate
(`gd32_bridge_resolve_wifi_bt()`) has already confirmed the bridge
understands them. Costs one small I2C frame/s on BRD_I2C (`i2c8`) while
any line is held as output.

As a consequence, `direction_output()` always reports success instead
of propagating the transfer error. This chip's consumers (the Display-1
panel `reset-gpios`, `mmc-pwrseq-simple`, a Bluetooth `shutdown-gpios`)
are ordinary `gpiod_direction_output()` callers, **not** OF `gpio-hog`
lines, and have no `-EPROBE_DEFER` equivalent once past `.request()`:
a nonzero return here would fail the *consumer's* own probe permanently
over what is usually only a transient bridge-down window. Swallowing it
instead lets the consumer's probe proceed; the requested state is
latched regardless and the replay applies it once the bridge answers,
so the consumer just cannot tell the line is actually driven until the
next successful replay (now logged with a `dev_warn_ratelimited()` at
the call site too).

Only the Linux side re-asserts. **#2297 stays open** for the CM33/Zephyr
side of the bridge, which does not yet re-apply anything after a reset,
and re-asserting REG_ON only restores the pin level — it does not
re-initialise the Linux Wi-Fi/BT drivers if the reset also disturbed the
Murata module itself.

See also: `docs/gd32-bridge-protocol.md` §3.1 (host-behaviour note,
including the BRD_I2C shared-bus/arbitration caveat, updated in the same
change).

**Follow-up (same branch; bug bench-observed 2026-09-26, E1M-V2M103,
this fix bench-verified 2026-09-26 on E1M-V2M103 with GD32_NRST held ~40 s past probe: bridge reachable ~46 s, SDIO card enumerated 48.4 s, brcmfmac firmware 49.7 s, hci0 UP+RUNNING 54.8 s, devices_deferred empty, no rebind; a normal boot is unchanged):** lines 18/19's `.request()` gate
(`gd32_bridge_resolve_wifi_bt()`) used to return `-EPROBE_DEFER` itself
when the bridge was still silent, which permanently deferred the
*consumer* -- `mmc-pwrseq-simple`'s `wlan-pwrseq` `reset-gpios` and
`hci_bcm`'s `shutdown-gpios` -- with nothing to ever retry it, since a
bare `-EPROBE_DEFER` only gets retried when some other driver's
(un)registration happens to walk the deferred-probe list. Observed on
the bench: NRST release failed, and
`/sys/kernel/debug/devices_deferred` listed `15c20000.mmc`,
`wlan-pwrseq` and `serial0-0` for the whole boot, even though the
bridge came up seconds later. `.request()` must still return
`-EPROBE_DEFER` rather than grant the request while unresolved,
though: `mmc-pwrseq-simple` and `hci_bcm` each run a one-shot power-up
sequence (`mmc_rescan()` / loading the `.hcd` patch) immediately once
`.request()` succeeds, and SDHI2 being non-removable means that
sequence never reruns on its own if it runs before REG_ON can actually
move.

What resolves the deferral instead: a new, independent `delayed_work`
(`gd32_bridge_resolve_work()`) that polls `GET_VERSION` regardless of
whether any consumer has requested the lines yet (flat at ~1 Hz for
the first ~30 s, then backing off exponentially to a 30 s cap; it
never gives up, since the bridge firmware can be reset or reflashed at
any point during a long-running boot). On resolving either way --
confirmed supported, or confirmed unsupported -- it briefly registers
a throwaway `platform_device` (`gd32_bridge_kick_deferred_probe()`)
purely so that device's bind runs `driver_bound()` -> the kernel's own
`driver_deferred_probe_trigger()` (`drivers/base/dd.c`, confirmed
present at that call site in this kernel tree) -- the only in-tree
hook that lets module code queue a deferred-probe retry (asynchronous,
on `system_unbound_wq`) on demand. That gets `mmc-pwrseq-simple` /
`hci_bcm` to retry `.request()` with the real answer -- granted, or
failing outright with `-ENODEV` -- instead of lingering deferred
indefinitely. `.request()` itself now makes only a single
non-blocking `GET_VERSION` attempt instead of a bounded blocking wait,
since the independent poller above is what actually catches a slow
bridge.
