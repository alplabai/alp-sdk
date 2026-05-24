# spi-master

Discrete SPI master.  Send a known byte pattern out MOSI, log
whatever clocks back on MISO.

Contrasts with [`examples/spi-loopback`](../spi-loopback/) -- that
example exercises both ends on the same chip (single-bus
self-test). This example is the production starting point:
replace the byte pattern with your chip's command + register
sequence.

## What this shows

* `alp_spi_open()` -- bus configuration (freq, mode, bits, CS).
* `alp_spi_write()` -- half-duplex TX-only (command-style write).
* `alp_spi_transceive()` -- full-duplex TX + RX (register read
  with in-band response).
* `alp_spi_read()` -- half-duplex RX-only (drain a streaming
  slave).
* `alp_spi_close()` -- clean shutdown.

## SPI quick reference

| Mode      | CPOL | CPHA | Clock idle | Sample edge      |
|-----------|------|------|------------|------------------|
| MODE_0    | 0    | 0    | Low        | Rising           |
| MODE_1    | 0    | 1    | Low        | Falling          |
| MODE_2    | 1    | 0    | High       | Falling          |
| MODE_3    | 1    | 1    | High       | Rising           |

MODE_0 is what most modern peripherals expect.  Check your
slave's datasheet -- a wrong mode produces "almost right" data
that's off by one bit.

## Wiring

The E1M-EVK routes `E1M_SPI1` to the Arduino UNO header SPI; the
SoM bridges the bus internally, so app code just opens the E1M
instance and never sees the physical termination.
For this example:

* No external slave required (you can verify by jumpering MOSI
  to MISO and confirming `transceive` echoes the TX pattern).
* For a real slave, connect SCK, MOSI, MISO, and either let the
  controller drive its own /CS or wire a free GPIO to the
  slave's /CS pin.
* Add a common ground between boards if the slave isn't on the
  same board.

## Build

```bash
# Standalone, native_sim (emul SPI; MISO reads back 0x00):
west build -b native_sim/native/64 examples/spi-master \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN701:
west build -b alp_e1m_aen701_m55_hp examples/spi-master
west flash
```

## Expected output

native_sim (no slave registered):

```
[spi-master] open E1M_SPI1 @ 1 MHz mode 0
[spi-master] write -> 0
[spi-master] transceive -> 0  rx={00 00 00 00}
[spi-master] read -> 0  rx={00 00 00 00}
[spi-master] done
```

Real hardware with MOSI -> MISO loopback jumper:

```
[spi-master] open E1M_SPI1 @ 1 MHz mode 0
[spi-master] write -> 0
[spi-master] transceive -> 0  rx={aa 55 de ad}
[spi-master] read -> 0  rx={ff ff ff ff}
[spi-master] done
```

Real hardware with a register-mapped slave at 0x10:

```
[spi-master] open E1M_SPI1 @ 1 MHz mode 0
[spi-master] write -> 0
[spi-master] transceive -> 0  rx={00 7a 5c 12}   <- slave's register echo
[spi-master] read -> 0  rx={5c 12 ab cd}         <- streaming data
[spi-master] done
```

## Customising

* **Different freq.**  Drop `.freq_hz` to 100 kHz for long /
  noisy traces; raise to 10 MHz once you've confirmed the
  slave's max and your wires are short.
* **Different mode.**  Most chips want MODE_0, but check their
  datasheet's timing diagram.
* **GPIO-driven CS.**  Replace `ALP_SPI_NO_CS` with a portable
  GPIO id routed on your board.  The wrapper drives the pin
  active-low around each transfer.
* **Larger words.**  Set `.bits_per_word = 16` or `32` -- the
  wrapper packs / unpacks for you.

## Reference

- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) SPI surface.
- [`examples/spi-loopback/`](../spi-loopback/) -- single-chip self-test.
- [`examples/spi-slave/`](../spi-slave/) -- slave-mode companion (API gap; see notes).
