# v2n-gd32-bridge-ping

Continuous PING + `GET_VERSION` liveness probe against the V2N
module's on-module GD32G553 supervisor MCU over the 25 MHz SPI fast
path.  (BRD_I2C is deliberately not opened -- the management-path
transport has its own HIL rows.)

## What it does

1. Opens the GD32 SPI bus at 25 MHz (the silicon-validated full-DMA
   slave / zero-interrupt polled-master configuration).
2. Calls `gd32g553_init(...)` in a 200 ms retry loop until the GD32
   answers -- the CM33 boots before the GD32 leaves reset (shared
   PMIC reset-out), so a one-shot init would race it.  The driver
   issues `PING` then `GET_VERSION` and refuses a major-version
   mismatch.
3. PINGs forever at 2 Hz; every 8th cycle re-reads the firmware
   version.  GET_VERSION's 7-byte reply is deliberately ODD-length --
   it stresses the GD32 reply path's FIFO-residue handling alongside
   the 4-byte (even) PING.

## Expected output

```
[gd32-bridge-ping] V2N supervisor MCU smoke test
[gd32-bridge-ping] init OK after 2 retries; firmware v0.2.0
[gd32-bridge-ping] SPI ping #0 -> 0
[gd32-bridge-ping] SPI get_version #0 -> 0 (v0.2.0)
[gd32-bridge-ping] SPI ping #1 -> 0
...
```

For full command-set coverage (GPIO/PWM/ADC/DAC/QENC/TRNG/TMU/...,
pass/fail accounting, transport recovery) see
[`v2n-gd32-bridge-hil-soak`](../v2n-gd32-bridge-hil-soak).

## See also

* [`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) — wire spec.
* [`docs/gd32-bridge.md`](../../../docs/gd32-bridge.md) — firmware tree overview.
* [`<alp/chips/gd32g553.h>`](../../../include/alp/chips/gd32g553.h) — host driver API.
