# 02 -- I²C bus scan

Walks `examples/i2c-scanner/` -- a 7-bit-address scan over an I²C
bus that prints the addresses that ACK.  Useful for confirming on-
module chip populations + smoke-testing a new carrier board's I²C
wiring.

## What it teaches

* How to open an `alp_i2c_t` against the board's primary I²C bus.
* The "ACK probe" idiom -- a zero-byte write whose ACK answers
  "is something at this address?"
* Why the SDK doesn't try to identify the chip at each address
  (use the chip driver's `_init` for that -- the scanner just
  reports addresses, not identities).

## Code path

```c
#include "alp/peripheral.h"

int main(void) {
    alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = 0u,   /* primary I²C */
        .bitrate_hz = 100000u,
    });
    if (!bus) return -1;

    for (uint8_t addr = 0x08; addr <= 0x77; ++addr) {
        /* Zero-byte write.  Backend translates to a START + ADDR + R/W=0
         * + STOP.  ACK = device present; NAK = nothing there. */
        if (alp_i2c_write(bus, addr, NULL, 0) == ALP_OK) {
            printf("0x%02X ACK\n", addr);
        }
    }
    alp_i2c_close(bus);
}
```

## Expected output on E1M-AEN701 + E1M-EVK

```
0x30 ACK   -- OPTIGA Trust M
0x48 ACK   -- TMP112
0x50 ACK   -- 24C128 EEPROM
0x52 ACK   -- RV-3028-C7 RTC
0x72 ACK   -- TCAL9538 I/O expander (EVK only)
```

## On V2N's BRD_I2C

Same code, swap `som.sku` to `E1M-V2N101`, rebuild.  Expected:

```
0x1C ACK   -- DA9292 PMIC
0x25 ACK   -- ACT88760 page 0
0x26 ACK   -- ACT88760 page 1
0x30 ACK   -- OPTIGA Trust M
0x40 ACK   -- TMP112
0x4D ACK   -- TPS628640 (optional)
0x52 ACK   -- RV-3028-C7
0x68 ACK   -- 5L35023B
0x70 ACK   -- GD32G553 bridge
```

If a documented address doesn't show up, the chip is missing or
mis-strapped -- compare against
`metadata/e1m_modules/<SKU>.yaml`'s `i2c_devices:` block.

## See also

* [`examples/i2c-scanner/`](../../examples/i2c-scanner/)
* [Tutorial 04 -- cross-family portability](04-cross-family-portability.md)
* [Tutorial 08 -- runtime board detection](08-runtime-board-detection.md)
