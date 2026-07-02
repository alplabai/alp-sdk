# spi-slave

Claim the bus in target (slave) mode using the portable
`alp_spi_target_*` surface from `<alp/peripheral.h>` (v0.9,
`[ABI-EXPERIMENTAL]`) and answer an external SPI controller.

## What it shows

* `alp_init()` -- SDK runtime bring-up before the first open.
* `alp_spi_target_open()` -- claim a bus in slave mode (the
  external controller owns SCK + /CS; no `cs_pin_id` on our side).
* `alp_spi_target_transceive()` -- the transfer-based slave idiom:
  preload a TX reply, block until the controller clocks a
  transfer, decode what arrived, preload the next reply.  SPI is
  full-duplex, so a slave can never answer a command within the
  SAME transfer -- replies always lag one frame.
* A tiny request/response protocol over 5-byte fixed frames:
  PING (`0x01`) echoes the payload, GET_VERSION (`0x02`) returns
  4 version bytes, unknown commands fill with `0xEE`.
* `alp_spi_target_close()` -- release the bus.

## Availability

Zephyr's SPI slave support is patchy -- some SoC controller
drivers reject `SPI_OP_MODE_SLAVE`.  Backends or drivers without
slave mode fail with `ALP_ERR_NOSUPPORT` (or `ALP_ERR_NOT_READY`
when the bus alias is unset), which the example handles by
printing the diagnostic and exiting -- that is the expected
outcome on **native_sim**, which has no slave-mode emulation:

```
[spi-slave] listening on BOARD_SPI_ARDUINO (mode 0, 8 bits)
[spi-slave] target open failed: alp_last_error=-2
[spi-slave]   SPI target (slave) mode is unavailable on this build
[spi-slave] done
```

## Test setup (real hardware)

1. Flash this example onto board A (the slave).
2. Adapt `examples/peripheral-io/spi-master` to send 5-byte frames:
   * `[0x01, 0xDE, 0xAD, 0xBE, 0xEF]` for PING (expect
     `[0x00, 0xDE, 0xAD, 0xBE, 0xEF]` back on the NEXT frame).
   * `[0x02, 0xFF, 0xFF, 0xFF, 0xFF]` for GET_VERSION (expect
     `[0x00, 0x00, 0x08, 0x00, 'A']` on the NEXT frame).
3. Wire SCK-SCK, MOSI-MOSI, MISO-MISO, /CS-/CS, GND-GND between
   the two boards.  The master drives SCK + MOSI + /CS; the slave
   drives MISO.
4. Power both boards.  Board A's console shows one `transfer N`
   line per frame board B clocks.

## Other useful tools

* **Logic analyser.**  Saleae Logic 2 (free SPI decoder) makes
  protocol bugs obvious -- CPOL/CPHA mismatches show up as
  off-by-one bit errors that decoded MOSI/MISO bytes make
  immediately visible.
* **Independent grounds** are a common bug source on
  inter-board SPI -- one missing GND between boards turns SPI
  edges into noise.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) -- the
  "SPI -- target (slave) mode" section documents the full contract.
- [`examples/peripheral-io/spi-loopback/`](../spi-loopback/) -- single-chip self-test.
- [`examples/peripheral-io/spi-master/`](../spi-master/) -- discrete master companion.
- Zephyr `spi_transceive` with `SPI_OP_MODE_SLAVE` -- the upstream
  mechanism the Zephyr backend dispatches through.
