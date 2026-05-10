# src/zephyr — Zephyr backend

Implementation of `<alp/peripheral.h>` etc. for Zephyr targets.  Only
compiled when `CONFIG_ALP_SDK=y` (Zephyr build) or when the parent
plain-CMake build was invoked with `-DALP_OS=zephyr` *and* `ZEPHYR_BASE`
is reachable.

## Lookup model

`alp_*_open()` takes a `bus_id` / `pin_id` / `port_id` from the
alp-studio pin allocator.  The Zephyr backend translates those
integer ids into Zephyr `struct device *` via **devicetree aliases**:

| API call                        | Expected DT alias               |
|---------------------------------|---------------------------------|
| `alp_i2c_open({.bus_id=N})`     | `&alp_i2cN`  (N = 0..7)         |
| `alp_spi_open({.bus_id=N})`     | `&alp_spiN`  (N = 0..7)         |
| `alp_uart_open({.port_id=N})`   | `&alp_uartN` (N = 0..7)         |
| `alp_gpio_open(N)`              | index N of the `alp,pin-array` node's `gpios` property |

Boards in `alplabai/alp-zephyr-modules` provide these aliases by
overlaying the SoC devicetree.  Example overlay snippet:

```dts
/ {
    aliases {
        alp-i2c0 = &i2c0;
        alp-spi0 = &spi0;
        alp-uart0 = &uart0;
    };

    alp_pins: alp-pins {
        compatible = "alp,pin-array";
        gpios = <&gpio0 1 GPIO_ACTIVE_HIGH>,
                <&gpio0 2 GPIO_ACTIVE_HIGH>,
                <&gpio0 3 GPIO_PULL_UP>;
    };
};
```

If an alias is absent, the corresponding `alp_*_open()` call returns
`NULL` — there is no runtime crash and no silent-NULL device pointer.

## Handle pools

Each peripheral kind gets a **static handle pool** sized by Kconfig
(`CONFIG_ALP_SDK_MAX_*_HANDLES`, default 4 each).  No heap is used.
`open()` claims a slot; `close()` releases it.  Concurrency is
serialised with a single `k_mutex` per pool.

## Source files

| File                  | Covers                       |
|-----------------------|------------------------------|
| `handles.h`           | Internal struct definitions  |
| `handles.c`           | Pool allocators              |
| `peripheral_i2c.c`    | I2C surface                  |
| `peripheral_spi.c`    | SPI surface (CS via gpio_dt_spec) |
| `peripheral_gpio.c`   | GPIO surface + IRQ glue      |
| `peripheral_uart.c`   | UART surface (poll-based v0.1) |

## Tests

`tests/zephyr/peripheral/` ships a ztest suite that runs under
`native_sim` using the in-tree `i2c-emul`, `spi-emul`, and emulated
GPIO controllers.  No real silicon required for CI.
