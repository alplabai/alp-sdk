# v2n-gd32-bridge-ping

PING + `GET_VERSION` + `GET_BUILD_ID` round-trip against the V2N
module's on-module GD32G553 supervisor MCU.  Cross-checks both
transports (SPI fast path + I2C management path) when wired.

## What it does

1. Opens `BRD_I2C` and the GD32 SPI bus.
2. Calls `gd32g553_init(...)` -- the driver issues `PING` then
   `GET_VERSION` and refuses to proceed on a major-version mismatch.
3. Issues a `ping` on each transport explicitly to confirm both
   are functional.
4. Reads the firmware build-id (truncated SHA-1 of the bridge
   firmware ELF) -- useful for production-test logging.
5. Reads the GD32-sampled DA9292 INT/TW fault-pin byte forwarded by
   the GD32.  (The GD32 has no I2C path to the DA9292; for
   register-level PMIC status read the DA9292 over BRD_I2C via
   `da9292_get_status()`.)

## Expected output

```
[gd32-bridge-ping] V2N supervisor MCU smoke test
[gd32-bridge-ping] init OK; firmware v0.1.0
[gd32-bridge-ping] SPI ping -> 0
[gd32-bridge-ping] I2C ping -> 0
[gd32-bridge-ping] firmware build-id: 0000000000000000abcd
[gd32-bridge-ping] DA9292 fault-pin state: 0xFF
[gd32-bridge-ping] done
```

The fault-pin byte `0xFF` is the firmware's "no DA9292 pin sample
taken yet" sentinel; pin sampling lands in a future firmware release,
after which this returns the GD32-observed `DA9292_INT`/`DA9292_TW`
pin state (bit0 = INT asserted, bit1 = TW asserted, bits 2-6
reserved).

## See also

* [`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) — wire spec.
* [`docs/gd32-bridge.md`](../../../docs/gd32-bridge.md) — firmware tree overview.
* [`<alp/chips/gd32g553.h>`](../../../include/alp/chips/gd32g553.h) — host driver API.
