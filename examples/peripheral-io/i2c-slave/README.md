# i2c-slave

Make this MCU answer on the bus as a register-mapped I2C target
(slave) using the portable `alp_i2c_target_*` surface from
`<alp/peripheral.h>` (v0.8, `[ABI-EXPERIMENTAL]`).

## What it shows

* `alp_init()` -- SDK runtime bring-up before the first open.
* `alp_i2c_target_open()` -- claim a bus as a target at a known
  7-bit address (`0x42`), with byte-granular ISR callbacks passed
  in the config:
  * `on_write` -- byte received from the external controller,
  * `on_read` -- byte requested by the external controller,
  * `on_stop` -- STOP condition (transaction boundary).
* The register-file pattern -- the idiom every embedded engineer
  expects for "make this MCU look like a sensor / EEPROM /
  register-mapped peripheral": first written byte latches the
  register pointer, subsequent bytes store with auto-increment,
  reads serve from the pointer with auto-increment, STOP re-arms
  the pointer latch.
* `alp_i2c_target_close()` -- unregister and release the bus.

## Availability

Target mode needs controller-driver support: on Zephyr that is
`CONFIG_I2C_TARGET` (set in this example's `prj.conf`) plus a
driver implementing `target_register`.  Backends or drivers
without it fail the open with `ALP_ERR_NOSUPPORT`, which the
example handles by printing the diagnostic and exiting -- that is
the expected outcome on **native_sim**, which has no target-mode
emulation:

```
[i2c-slave] listening @ 0x42 on BOARD_I2C_SENSORS
[i2c-slave] target open failed: alp_last_error=-6
[i2c-slave]   I2C target mode is unavailable on this build
[i2c-slave] done
```

## Test setup (real hardware)

1. Flash this example onto board A.
2. Flash a master example (modify `examples/peripheral-io/i2c-master`
   to point at address `0x42` and use a simple register read/write
   loop) onto board B -- or probe from a USB-I2C adapter:
   `i2ctransfer -y 0 w1@0x42 0x00 r4` should return
   `0xa0 0xa1 0xa2 0xa3`.
3. Wire SDA-SDA, SCL-SCL, GND-GND between the two boards.  Use
   external 4.7 kΩ pull-ups on SDA + SCL if neither board
   provides them.
4. Power both boards.  Board A's console shows `writes_seen`
   incrementing as board B sends register writes; board B reads
   back the values board A primed into `g_regs[]`.

## Other useful tools

* **Logic analyser.**  Saleae Logic 2 (free decoder for I2C) +
  any cheap clone shows the bus traffic.  Essential when debugging
  bus contention or NACKs.
* **`i2c-scanner`** running on board B with no target attached gives
  a baseline "what's on the bus" reading -- run it before flipping
  on the target to confirm address 0x42 isn't already taken.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) -- the
  "I2C -- target (slave) mode" section documents the full contract.
- [`examples/peripheral-io/i2c-scanner/`](../i2c-scanner/) -- master-side discovery.
- [`examples/peripheral-io/i2c-master/`](../i2c-master/) -- master-side known-address read.
- Zephyr `i2c_target_register` -- the upstream API the Zephyr
  backend dispatches through.
