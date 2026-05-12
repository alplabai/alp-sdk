# 05 -- The supervisor MCU bridge (V2N)

Walks `examples/v2n-gd32-bridge-ping/` and explains the hybrid
SPI + I²C bridge between the Renesas RZ/V2N host and the on-module
GD32G553 supervisor MCU.

## Why a second MCU?

The V2N SoM allocates some E1M-edge peripherals to a companion
GD32G553 because they don't fit on the Renesas RZ/V2N pinmux.
After the 2026-05-11 schematic revision, two of the eight E1M PWM
channels are GD32-only (PWM6 + PWM7), as is the cached DA9292 PMIC
status forwarder + several auxiliary GPIO routes.

## The two transports

Both carry the same command set; the host picks per-call:

| Transport | Renesas pads      | GD32 pads          | Speed         | Use case                           |
|-----------|-------------------|---------------------|---------------|------------------------------------|
| SPI       | P76/77/96/97      | PA8/9/10/PB15       | up to 25 MHz  | low-latency telemetry, frequent PWM updates |
| I²C       | P07/P06 (BRD_I2C) | PA15/PB9 @ 0x70    | up to 1 MHz   | when already on BRD_I2C for PMICs  |

## The command set

| Opcode | Helper                           | What it does                          |
|--------|----------------------------------|---------------------------------------|
| 0x00   | `gd32g553_ping`                  | Liveness probe                        |
| 0x01   | `gd32g553_get_version`           | Firmware version triple               |
| 0x02   | `gd32g553_get_build_id`          | 20-char SHA-1 truncation              |
| 0x03   | `gd32g553_get_reset_reason`      | Why the GD32 last reset               |
| 0x10/11 | `gd32g553_gpio_read/write`      | Masked GD32-side GPIO access          |
| 0x20/21 | `gd32g553_pwm_set/get`          | GD32-side PWM channel control         |
| 0x30   | `gd32g553_adc_read`              | GD32-side ADC samples                 |
| 0x40   | `gd32g553_da9292_status_forward` | Cached DA9292 PMIC status byte       |
| 0xF0..0xF6 | `gd32g553_ota_*`             | Application-bootloader OTA opcodes    |

## The example's code path

```c
alp_spi_t *spi = alp_spi_open(&(alp_spi_config_t){
    .bus_id = 0u, .freq_hz = 10000000u, .mode = ALP_SPI_MODE_0, ...
});

gd32g553_t gd32;
alp_status_t s = gd32g553_init(&gd32, spi, NULL, 0u);
/* init runs PING + GET_VERSION; refuses to operate on mismatched
   firmware major version */

printf("bridge v%u.%u.%u\n", gd32.version.major, gd32.version.minor, gd32.version.patch);
gd32g553_ping(&gd32);
```

## Why this matters for portability

If you write firmware that talks to the GD32 directly, it's a
Ring-3 (SoM-bound) application -- it only works on V2N.  Most
applications don't need to touch the GD32; they use the SDK's
peripheral abstractions and the SDK figures out which side of the
bridge owns each pin.

## See also

* [`docs/gd32-bridge-protocol.md`](../gd32-bridge-protocol.md) --
  the wire spec.
* [`<alp/chips/gd32g553.h>`](../../include/alp/chips/gd32g553.h) --
  host-side driver header.
* [Tutorial 07](07-recovering-a-bricked-bridge.md) -- when the bridge
  firmware breaks.
