# uart-rx-ringbuf

Per-feature example for `<alp/peripheral.h>`'s opt-in
interrupt-driven RX ring buffer (`CONFIG_ALP_SDK_UART_RX_RINGBUF`).

## What this shows

- Why the IRQ-driven ring buffer exists (and when *not* to use it).
- Attaching an LwRB-backed ring to an open `alp_uart_t`.
- Draining batched bytes from the consumer thread without
  blocking the I/O path.
- Sizing the backing store against worst-case drain latency.
- Detaching cleanly.

## When to reach for this vs `alp_uart_read`

- **`alp_uart_read`** — synchronous, blocks the caller until bytes
  arrive or the timeout expires.  Fine for command prompts +
  scripted interactions.
- **`alp_uart_rx_ringbuf_*`** — interrupt-driven, decouples the
  RX byte arrival from any specific thread.  Use when the app
  needs to do other work in parallel: chatty sensors at low baud
  rates, debug shells multiplexed with bus traffic, GPS / NMEA
  streams parsed once per second.

## Build

```bash
west build -b native_sim/native/64 examples/peripheral-io/uart-rx-ringbuf \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) — the
  `alp_uart_rx_ringbuf_*` API surface.
- [`vendors/lwrb/`](../../vendors/lwrb/) — the ring-buffer
  library the helper is built on.
