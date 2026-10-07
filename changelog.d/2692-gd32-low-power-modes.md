### Added — GD32 bridge low-power modes: `gd32g553_set_power_mode()` with Deep-sleep wake handling and BUSY gating

`CMD_POWER_MODE_SET` (`0x28`) now has a host API that handles the wake. `gd32g553_set_power_mode(ctx, mode, opts)`
takes the wake sources, an optional `GD32G553_POWER_FLAG_WAKE_I2C` (an early wake of a timed Deep-sleep on a BRD_I2C
address match) and wake tuning; `gd32g553_power_wake()` is the wake procedure on its own. An untimed `WAKE_I2C`
request is refused (`ALP_ERR_OUT_OF_RANGE`), host and firmware alike, until the I2C0 wake line is bench-proven. After
an accepted Deep-sleep the next command first runs the wake: a throw-away PING (a CS wake loses the frame clocked
during it), a wake-latency wait (default 2 ms) and a RUN request that also cancels a request which never reached the
sleep; if every attempt fails the context keeps assuming the bridge is asleep and returns the transport error.
STANDBY invalidates the negotiated link state like an OTA reset. The firmware answers `ALP_ERR_BUSY` for modes 2 and
3 while an ADC stream, PWM output or capture, DAC output, OTA session or unconfirmed trial is live. The opcode is SPI
only. Request byte 1 was reserved padding; a pre-flags host sends 0 and is unaffected.

**Layout change:** `gd32g553_t` gains three fields before the SPI scratch buffers (`power_asleep`,
`power_wake_latency_us`, `power_wake_retries`), so `sizeof(gd32g553_t)` grows; rebuild anything that embeds one. See
`docs/gd32-bridge-protocol.md` §3.z.

`v2n-gd32-bridge-functional` gains rows 29-31 (`power_deep_sleep_wake`, `power_busy_gate`, `power_invalid`): a 250 ms
timed Deep-sleep with the documented wake and a `GET_VERSION` that must succeed, the `ALP_ERR_BUSY` gate with a PWM
channel claimed, and the `ALP_ERR_INVAL` rejections of an untimed Deep-sleep and an unknown mode. They skip on a bridge
below protocol minor 17 and never request STANDBY.
