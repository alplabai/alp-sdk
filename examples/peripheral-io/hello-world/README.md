# hello-world

The canonical "first program" for the Alp SDK.  No peripherals,
no chips, no board-specific wiring -- just a periodic `printf`
loop so you can confirm the toolchain, flash flow, and log
console are wired correctly before chasing harder bugs.

If `[hello] tick 0` doesn't appear, the suspect list is short:

1.  **Toolchain** -- the build output completes cleanly but the
    image you flashed doesn't match the SoM (cross-compiled for
    the wrong core; mismatched Zephyr board target).
2.  **Flash flow** -- the image flashed but boot vector / SAU /
    secure-state config doesn't allow it to start.
3.  **Console wiring** -- the app is running but your terminal is
    on the wrong UART, wrong baud rate, or wrong USB enumeration.

Pin down which one BEFORE moving on to `gpio-button-led` or
`i2c-scanner`.

## What this shows

* The minimal `board.yaml` v2 shape: `schema_version`, `som.sku`,
  `board.name`, one `cores.<id>` with an empty `peripherals: []`.
* That `prj.conf` stays empty -- all CONFIG_* selection comes from
  the loader-generated `alp.conf`.
* The Zephyr boot -> `main()` -> printf path with no peripheral
  initialisation in between.

## Build

```bash
# Standalone, native_sim (host binary; no hardware needed):
west build -b native_sim/native/64 examples/peripheral-io/hello-world \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN701:
west build -b alp_e1m_aen701_m55_hp examples/peripheral-io/hello-world
west flash
```

## Where the output lands

| SoM family   | Console route                                            |
|--------------|----------------------------------------------------------|
| E1M-AEN      | Alif first USART -> FTDI USB-UART on E1M-EVK (115200 8N1) |
| E1M-V2N      | Renesas SCIF -> FTDI USB-UART on E1M-X-EVK (115200 8N1)   |
| E1M-V2N-M1   | Same as V2N                                              |
| native_sim   | Host binary stdout                                       |

Open the serial port in your terminal of choice (115200 8N1).
The recommended cross-platform tool is `tio` -- one binary, one
syntax on Linux, macOS, and Windows:

```
tio -b 115200 <your-serial-device>
```

Per-OS device naming (and the alternative terminals each OS
prefers) is documented in
[`docs/cross-platform-setup.md`](../../../docs/cross-platform-setup.md)
section 7.7.

<!-- cross-platform-lint:ignore -->
Quick reference: Linux uses `/dev/ttyUSB*` or `/dev/ttyACM*`;
macOS uses `/dev/cu.usbserial-*` or `/dev/cu.usbmodem*`; Windows
uses `COM<N>` (visible in Device Manager under "Ports (COM &
LPT)").
<!-- cross-platform-lint:resume -->

## Expected output

```
[hello] Alp SDK hello-world starting
[hello] tick 0
[hello] tick 1
[hello] tick 2
[hello] tick 3
[hello] tick 4
[hello] done
```

## Customising

* **Loop forever.**  Swap the bounded `for` loop in `src/main.c`
  for the commented-out `TICKS_ON_REAL_SILICON` block.
* **Faster heartbeat.**  Drop `HELLO_TICK_PERIOD_MS` from `1000`
  to `100` (10 Hz) or `10` (100 Hz).  At <= 1 ms you'll start to
  see scheduler overhead.
* **Different log macro.**  Swap `printf` for `printk` (always
  ISR-safe) or Zephyr's `LOG_INF` (compile-time-filtered,
  requires `CONFIG_LOG=y` in `prj.conf`).

## Reference

- [`docs/firmware-quickstart.md`](../../../docs/firmware-quickstart.md)
  -- single-OS bring-up walk-through.
- [`docs/cross-platform-setup.md`](../../../docs/cross-platform-setup.md)
  -- Windows/macOS/Linux toolchain + serial-port notes.
- [`docs/troubleshooting.md`](../../../docs/troubleshooting.md)
  -- common boot/flash/console failures + fixes.
