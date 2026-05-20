# uart-hello-world

The canonical "printf via UART" walkthrough.  Opens a UART port
with `alp_uart_open()`, sends a greeting + a monotonically-
increasing counter every second, demonstrates error handling.

Distinct from [`examples/uart-echo`](../uart-echo/) -- that
example is a bidirectional ping-pong; this one is the
producer-only variant most vendor SDKs ship as their first
tutorial.

## Why not just `printf`?

Zephyr's `printf` lands on the *console* UART -- a single,
Kconfig-pinned device.  Many real apps need to drive a
*different* UART:

* Bluetooth modem on a secondary serial port.
* GPS module on a board-specific header.
* Stepper-motor driver expecting a stream of TMC2209 datagrams.
* Debug pin on a custom board that isn't the console.

`alp_uart_*()` lets you open ANY UART by portable instance ID
(`E1M_UART0`, `E1M_UART1`) without touching `CONFIG_CONSOLE_*`
knobs.  It also takes a byte buffer + length, which is what you
want when shipping binary protocols (`printf` is text-only).

## What this shows

* `alp_uart_open()` -- opens `E1M_UART0` at 115200 8N1.
* `alp_uart_write()` -- blocking byte-buffer write.
* Error handling via `alp_last_error()` when the port can't open.
* `alp_uart_close()` -- clean shutdown.

## Build

```bash
# Standalone, native_sim (host binary -- writes go to stdout):
west build -b native_sim/native/64 examples/uart-hello-world \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN701:
west build -b alp_e1m_aen701_m55_hp examples/uart-hello-world
west flash
```

## Attaching a terminal

The output appears on whatever the board wires `E1M_UART0` to.
On the E1M-EVK and E1M-X-EVK that's the on-board FTDI USB-UART
(115200 8N1, 8 data bits, no parity, 1 stop bit).

Recommended cross-platform invocation (one binary, one syntax,
on Linux + macOS + Windows):

```
tio -b 115200 <your-serial-device>
```

Per-OS device naming + alternative terminals (screen, minicom,
picocom, PuTTY) are documented in
[`docs/cross-platform-setup.md`](../../docs/cross-platform-setup.md)
section 7.7.

<!-- cross-platform-lint:ignore -->
Quick reference table (replace `<port>` with your OS-specific
device path):

| OS         | Tool       | Command                                              |
|------------|------------|------------------------------------------------------|
| Linux      | screen     | `screen /dev/ttyUSB0 115200`                         |
| Linux      | minicom    | `minicom -D /dev/ttyUSB0 -b 115200`                  |
| Linux      | picocom    | `picocom -b 115200 /dev/ttyUSB0`                     |
| macOS      | screen     | `screen /dev/tty.usbserial-* 115200`                 |
| Windows    | PuTTY      | Connection type: Serial, Line: `COM<N>`, Speed: 115200 |
| Windows    | tio        | `tio -b 115200 COM3` (after `scoop install tio`)     |
| Cross      | tio        | `tio -b 115200 <port>` (recommended -- single binary) |
<!-- cross-platform-lint:resume -->

## Expected output

On the SDK log console (e.g. RTT or another UART):

```
[uart-hello] open E1M_UART0 @ 115200 8N1
[uart-hello] greeting written
[uart-hello] tick 0 written
[uart-hello] tick 1 written
...
[uart-hello] done
```

On the terminal attached to E1M_UART0 itself:

```
ALP SDK uart-hello-world
tick 0
tick 1
tick 2
tick 3
tick 4
```

## Customising

* **Different baud rate.**  Change `.baudrate = 115200` to
  `9600` (legacy GPS), `460800` (faster log streams), etc.
  The wrapper rounds to the controller's closest achievable
  rate and returns NOSUPPORT if the request is unreachable.
* **Different framing.**  Override `.data_bits`, `.parity`,
  `.stop_bits` for industrial 7-E-1 / RS-485 9-bit / ...
* **Different port.**  `E1M_UART0` is the conventional console;
  swap for `E1M_UART1` for a secondary serial route.

## Reference

- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) UART surface.
- [`examples/uart-echo/`](../uart-echo/) -- bidirectional companion.
- [`examples/uart-rx-ringbuf/`](../uart-rx-ringbuf/) -- IRQ-driven RX into a ring buffer.
- [`docs/cross-platform-setup.md`](../../docs/cross-platform-setup.md)
  -- Windows/macOS/Linux serial-port setup.
