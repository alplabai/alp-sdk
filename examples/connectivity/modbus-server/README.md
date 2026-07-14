# modbus-server

Modbus teaching example for the ADR 0018 `libraries: [modbus]` manifest.

The example runs one Zephyr Modbus client and one Zephyr Modbus server in the
same process using the RAW_ADU backend.  That keeps the example hardware-free:
native_sim can exercise the Modbus register model without a UART, RS-485
transceiver, CAN adapter, or TCP/IP stack.

`board.yaml` selects the curated library.  `scripts/alp_project.py` reads
`metadata/libraries/modbus.yaml` and emits `CONFIG_MODBUS=y`; `prj.conf` adds
the transport choice (`CONFIG_MODBUS_RAW_ADU=y` with two RAW interfaces).

## Build

```bash
west build -b native_sim/native/64 examples/connectivity/modbus-server \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

Expected output:

```text
[modbus] RAW_ADU server example starting
[modbus] holding[0]=0x4d42
[modbus] coils[0..7]=0x01
[modbus] done
```

## Moving To Serial

For RTU or ASCII on real hardware, keep the server callbacks in `src/main.c`
and replace the RAW_ADU setup with a UART-backed Modbus interface.  The board
devicetree must provide an enabled node with compatible `zephyr,modbus-serial`;
Zephyr's `CONFIG_MODBUS_SERIAL` depends on that node and a UART driver.
