### Added — GD32 bridge low-power modes: `gd32g553_set_power_mode()` with wake handling, BUSY gating and an I2C wake flag

`CMD_POWER_MODE_SET` (`0x28`) now has a host API that handles the wake. `gd32g553_set_power_mode(ctx, mode, opts)`
takes the wake sources, an optional `GD32G553_POWER_FLAG_WAKE_I2C` (a BRD_I2C address match ends a Deep-sleep, so it
may carry no timer) and wake tuning. After an accepted Deep-sleep the next command first runs `gd32g553_power_wake()`:
a throw-away PING (a CS wake loses the frame clocked during it), a wake latency wait (default 2 ms) and a RUN request
that also cancels a request which never reached the sleep. STANDBY invalidates the negotiated link state like an OTA
reset. The firmware answers `ALP_ERR_BUSY` for modes 2 and 3 while an ADC stream, PWM output or capture, DAC output,
OTA session or unconfirmed trial is live. The opcode is SPI only. Request byte 1 was reserved padding; a pre-flags
host sends 0 and is unaffected. See `docs/gd32-bridge-protocol.md` §3.z.
