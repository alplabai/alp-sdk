# V2N / V2N-M1-specific examples

Reference applications that exercise E1M-X-V2N-family hardware
(Renesas RZ/V2N A55 + dual-M33 + DRP-AI NPU, GD32G553 supervisor
MCU, on-module PMIC fleet, OPTIGA Trust M secure element, eMMC +
xSPI NOR storage, dual Gigabit Ethernet, on-module RTC + temp
sensor).  Build any of these against an E1M-X-V2N SKU on the
E1M-X-EVK carrier.

V2N-M1 SKUs add the DEEPX-DXM1 NPU rail; the bring-up delta is
captured in [`docs/bring-up-v2n-m1.md`](../../docs/bring-up-v2n-m1.md).

| Directory                                              | What it shows                                                                |
|--------------------------------------------------------|------------------------------------------------------------------------------|
| [`v2n-gd32-bridge-ping`](v2n-gd32-bridge-ping/)        | PING + GET_VERSION handshake against the GD32 supervisor over both SPI (fast path) and I²C (management path). The canonical first build on V2N. |
| [`v2n-board-id-readout`](v2n-board-id-readout/)        | Read the 128-byte SoM EEPROM manifest + assert the runtime SKU matches the firmware build's expected SKU.                                       |
| [`v2n-eeprom-manifest-dump`](v2n-eeprom-manifest-dump/)| Hexdump + structured decode of the EEPROM manifest at offset 0x0000 (magic, schema_v1, family, sku, hw_rev, serial, mfg_date, CRC32).            |
| [`v2n-ethernet-dual`](v2n-ethernet-dual/)              | Bring up both RTL8211FDI PHYs (ET0 + ET1) -- probe, reset, autoneg, link, Wake-on-LAN config.                                                    |
| [`v2n-rtc-multi-alarm`](v2n-rtc-multi-alarm/)          | Register per-source callbacks on the rv3028c7 multi-source alarm dispatcher (timer + periodic + clock-out + manual).                             |
| [`v2n-temp-sensor`](v2n-temp-sensor/)                  | Read the on-module TMP112 once per second and print degrees Celsius.                                                                             |
| [`v2n-pwm-fan-control`](v2n-pwm-fan-control/)          | Ramp a GD32-side PWM channel along a five-stop fan curve (25 kHz carrier, 0--100 % duty interpolation).                                          |
| [`v2n-secure-element-sign`](v2n-secure-element-sign/)  | OPTIGA Trust M init + product info readout + raw-APDU ECDSA-P256 sign over a 32-byte digest.                                                     |
| [`v2n-xspi-flash-readwrite`](v2n-xspi-flash-readwrite/)| Erase + write + read-back one page on the on-module xSPI NOR.                                                                                    |
| [`v2n-emmc-block-stat`](v2n-emmc-block-stat/)          | Disk-access ioctls + first-block read on the on-module eMMC.                                                                                     |
| [`v2n-gd32-swd-flash`](v2n-gd32-swd-flash/)            | Host-driven SWD bit-bang on the GD32 supervisor -- connect, halt, mass-erase, program, verify, reset.                                            |

## Why a separate index here

The top-level [`examples/README.md`](../README.md) lists every
example.  This sub-index exists because V2N-specific examples
need V2N-family hardware (GD32 supervisor, OPTIGA, dual PHY,
on-module RTC) that doesn't exist on AEN or N93.  Keeping them
under `examples/v2n/` makes that constraint visible from the
filesystem layout alone.  Cross-family examples (gpio, i2c,
pwm, audio, ...) stay at the top level of `examples/`.

## See also

- [`docs/soms/v2n.md`](../../docs/soms/v2n.md) -- V2N SoM
  one-pager + supported peripherals.
- [`docs/soms/v2n-m1.md`](../../docs/soms/v2n-m1.md) -- V2N-M1
  delta (DEEPX rail bring-up).
- [`docs/bring-up-v2n.md`](../../docs/bring-up-v2n.md) --
  step-by-step V2N hardware bring-up flow.
- [`docs/bring-up-v2n-m1.md`](../../docs/bring-up-v2n-m1.md) --
  DEEPX-rail addendum for V2N-M1 SKUs.
- [`docs/gd32-bridge.md`](../../docs/gd32-bridge.md) +
  [`docs/gd32-bridge-protocol.md`](../../docs/gd32-bridge-protocol.md)
  -- the GD32 supervisor protocol the v2n-gd32-* examples
  exercise.
