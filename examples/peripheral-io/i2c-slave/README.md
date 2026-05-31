# i2c-slave

Demonstrate the *shape* of I2C slave-mode application code on the
Alp SDK.

## SDK gap notice

**As of v0.6 the Alp SDK does NOT support I2C slave mode through
`<alp/peripheral.h>`.**  The header exposes master-only calls
(`alp_i2c_open`, `alp_i2c_write`, `alp_i2c_read`,
`alp_i2c_write_read`).  Slave-mode support is planned for v0.7.

This example exists to:

1. **Document the gap** so customers don't waste time hunting for
   a non-existent header.
2. **Stake out the proposed API shape** so when the slave-mode
   surface lands, this example is the migration template.
3. **Show the recommended register-file pattern** -- the idiom
   every embedded engineer expects for "make this MCU look like
   a sensor / EEPROM / register-mapped peripheral".

The code in `src/main.c` defines a local `alp_i2c_slave_*` shim
that returns `ALP_ERR_NOSUPPORT` from every call.  When the real
surface lands, delete the shim block and the downstream
application code keeps compiling against the upstream names.

## What this WILL show (once the API lands)

* `alp_i2c_slave_open()` -- claim a bus as slave at a known
  7-bit address.
* `alp_i2c_slave_set_callbacks()` -- register write-from-master
  and read-from-master ISR callbacks.
* The fake-register-file pattern: an array of bytes that the
  callbacks read/write to make the MCU behave like a sensor.
* `alp_i2c_slave_close()` -- release the bus.

## What this DOES show today

```
[i2c-slave] open as slave @ 0x42 on E1M_I2C0
[i2c-slave] Alp SDK v0.6 does NOT support I2C slave mode
[i2c-slave]   <alp/peripheral.h> is master-only today
[i2c-slave]   tracking: v0.7 API surface addition
[i2c-slave] done
```

So far so good: the example *compiles* against the proposed names
(via the shim), exits cleanly with a diagnostic, and gives
customers a copy-paste starting point for when the API lands.

## Test setup (once the API lands)

To exercise I2C slave mode end-to-end on real hardware:

1. Flash this example onto board A.
2. Flash a master example (modify `examples/peripheral-io/i2c-master` to point
   at address `0x42` and use a simple register read/write loop)
   onto board B.
3. Wire SDA-SDA, SCL-SCL, GND-GND between the two boards.  Use
   external 4.7 kΩ pull-ups on SDA + SCL if neither board
   provides them.
4. Power both boards.  Board A's console should show
   `writes_seen` incrementing as board B sends register writes;
   board B should see register read values matching what board A
   primed into `g_regs[]`.

## Other useful tools

* **Logic analyser.**  Saleae Logic 2 (free decoder for I2C) +
  any cheap clone shows the bus traffic.  Essential when debugging
  bus contention or NACKs.
* **`i2c-scanner`** running on board B with no slave attached gives
  a baseline "what's on the bus" reading -- run it before flipping
  on the slave to confirm address 0x42 isn't already taken.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) I2C surface (master-only today).
- [`examples/peripheral-io/i2c-scanner/`](../i2c-scanner/) -- master-side discovery.
- [`examples/peripheral-io/i2c-master/`](../i2c-master/) -- master-side known-address read.
- Zephyr `i2c_slave_register` -- the upstream API the future
  `alp_i2c_slave_*` will dispatch through on the Zephyr backend.
