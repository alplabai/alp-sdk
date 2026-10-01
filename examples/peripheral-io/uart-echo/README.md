# uart-echo

Per-peripheral example for `<alp/peripheral.h>` UART.  Reads
single bytes from the UART and writes them back.

## What this shows

- Opening a UART port by portable port ID (`ALP_E1M_UART0`).
- Synchronous read with timeout via `alp_uart_read`.
- Mirror-write back via `alp_uart_write`.

## Build

```bash
# writes examples/peripheral-io/uart-echo/generated/alp.conf, which west reads below (#866)
python3 scripts/gen_example_alp_conf.py examples/peripheral-io/uart-echo
west build -b native_sim/native/64 examples/peripheral-io/uart-echo \
    -- -DEXTRA_CONF_FILE=generated/alp.conf -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) UART surface
- [`examples/peripheral-io/uart-rx-ringbuf/`](../uart-rx-ringbuf/) — companion example for
  the opt-in IRQ-driven `alp_uart_rx_ringbuf_*` API.  Reach for that when
  the app needs to do other work while bytes trickle in.
