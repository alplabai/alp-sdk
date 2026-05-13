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
5. Reads the cached DA9292 PMIC status byte forwarded by the
   GD32.

## Expected output

```
[gd32-bridge-ping] V2N supervisor MCU smoke test
[gd32-bridge-ping] init OK; firmware v0.1.0
[gd32-bridge-ping] SPI ping -> 0
[gd32-bridge-ping] I2C ping -> 0
[gd32-bridge-ping] firmware build-id: 0000000000000000abcd
[gd32-bridge-ping] DA9292 cached status: 0xFF
[gd32-bridge-ping] done
```

The PMIC status byte `0xFF` is the firmware's "no PMIC poll has
populated the cache yet" sentinel; production firmware (once the
HAL backend lands) will return a real status snapshot.

## See also

* [`docs/gd32-bridge-protocol.md`](../../../docs/gd32-bridge-protocol.md) — wire spec.
* [`docs/gd32-bridge.md`](../../../docs/gd32-bridge.md) — firmware tree overview.
* [`<alp/chips/gd32g553.h>`](../../../include/alp/chips/gd32g553.h) — host driver API.
