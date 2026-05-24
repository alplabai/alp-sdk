# spi-slave

Demonstrate the *shape* of SPI slave-mode application code on the
ALP SDK.

## SDK gap notice

**As of v0.6 the ALP SDK does NOT support SPI slave mode through
`<alp/peripheral.h>`.**  The header exposes master-only calls
(`alp_spi_open`, `alp_spi_write`, `alp_spi_read`,
`alp_spi_transceive`).  Slave-mode support is planned for v0.7.

Note: Zephyr's own SPI slave support is itself patchy -- many SoC
drivers don't implement `spi_slave_register`.  When the ALP SDK
slave-mode wrapper lands, expect `ALP_ERR_NOSUPPORT` on backends
whose upstream driver hasn't implemented slave mode yet.

This example exists to:

1. **Document the gap** so customers don't waste time hunting for
   a non-existent header.
2. **Stake out the proposed API shape** so when the slave-mode
   surface lands, this example is the migration template.
3. **Show the recommended callback-based state-machine pattern** --
   the idiom every embedded engineer expects for "respond to
   whatever the master clocks in".

The code in `src/main.c` defines a local `alp_spi_slave_*` shim
that returns `ALP_ERR_NOSUPPORT` from every call.  When the real
surface lands, delete the shim block and the downstream
application code keeps compiling against the upstream names.

## What this WILL show (once the API lands)

* `alp_spi_slave_open()` -- claim a bus as slave.
* `alp_spi_slave_set_callbacks()` -- register per-byte and
  end-of-transfer ISR callbacks.
* A tiny request/response protocol: PING (0x01) echoes payload,
  GET_VERSION (0x02) returns a 4-byte version string.
* `alp_spi_slave_close()` -- release the bus.

## What this DOES show today

```
[spi-slave] open as slave on E1M_SPI1 (mode 0, 8 bits)
[spi-slave] ALP SDK v0.6 does NOT support SPI slave mode
[spi-slave]   <alp/peripheral.h> is master-only today
[spi-slave]   Note: Zephyr's own SPI slave support is patchy too;
[spi-slave]         some SoC drivers don't implement spi_slave_register.
[spi-slave]   tracking: v0.7 API surface addition
[spi-slave] done
```

## Test setup (once the API lands)

To exercise SPI slave mode end-to-end on real hardware:

1. Flash this example onto board A (the slave).
2. Adapt `examples/spi-master` to send the protocol bytes:
   * `[0x01, 0xDE, 0xAD, 0xBE, 0xEF]` for PING (expect
     `[0x00, 0xDE, 0xAD, 0xBE, 0xEF]` back on MISO).
   * `[0x02, 0xFF, 0xFF, 0xFF, 0xFF]` for GET_VERSION (expect
     `[0x00, 0x00, 0x06, 0x00, 'A']`).
3. Wire SCK-SCK, MOSI-MOSI, MISO-MISO, /CS-/CS, GND-GND between
   the two boards.  The master drives all four lines; the slave
   listens.
4. Power both boards.  Board A's console should show `transfers`
   incrementing as board B sends commands.

## Other useful tools

* **Logic analyser.**  Saleae Logic 2 (free SPI decoder) makes
  protocol bugs obvious -- CPOL/CPHA mismatches show up as
  off-by-one bit errors that decoded MOSI/MISO bytes make
  immediately visible.
* **Independent grounds** are a common bug source on
  inter-board SPI -- one missing GND between boards turns SPI
  edges into noise.

## Reference

- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) SPI surface (master-only today).
- [`examples/spi-loopback/`](../spi-loopback/) -- single-chip self-test.
- [`examples/spi-master/`](../spi-master/) -- discrete master companion.
- Zephyr `spi_slave_register` -- the upstream API the future
  `alp_spi_slave_*` will dispatch through on the Zephyr backend.
