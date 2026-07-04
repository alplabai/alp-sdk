# i2c-slave

Make this MCU answer on the bus as a register-mapped I2C target
(slave) using the register-file helper from `<alp/i2c_regfile.h>`
(v0.9, `[ABI-EXPERIMENTAL]`), a pure layer over the portable
`alp_i2c_target_*` surface in `<alp/peripheral.h>`.

## What it shows

* `alp_init()` -- SDK runtime bring-up before the first open
  (checked; the example bails out on failure).
* `alp_i2c_regfile_open()` -- claim a bus as a target at a known
  7-bit address (`0x42`) and expose a caller-owned buffer as the
  classic register file.  The helper ships the state machine every
  target application used to hand-roll: first written byte latches
  the register pointer, subsequent bytes store with wraparound
  auto-increment, reads serve from the pointer, STOP re-arms the
  pointer latch.
* `alp_i2c_regfile_set_write_window()` -- carve out read-only
  registers (device-ID style) by shrinking the controller-writable
  window.
* `alp_i2c_regfile_stats()` -- traffic counters for bench
  observability (`writes_seen` / `reads_seen`).
* `alp_i2c_regfile_close()` -- unregister and release the bus.

Need a shape the register-file idiom doesn't fit (command/response
protocols, FIFOs)?  Drop down to the raw byte-granular callbacks:
`alp_i2c_target_open()` with `on_write` / `on_read` / `on_stop` in
`<alp/peripheral.h>`.  This example's git history shows the same
register file hand-rolled on those callbacks.

## Availability

Target mode needs controller-driver support: on Zephyr that is
`CONFIG_I2C_TARGET` (set in this example's `prj.conf`) plus a
driver implementing `target_register`.  The helper degrades exactly
as `alp_i2c_target_open` does -- backends or drivers without target
mode fail the open with `ALP_ERR_NOSUPPORT`, which the example
handles by printing the diagnostic and exiting.

On **native_sim** (the CI lane) the emulated controller accepts
the registration, but no external controller ever drives the
emulated bus -- the ticks simply show `writes_seen=0`:

```
[i2c-slave] listening @ 0x42 on BOARD_I2C_SENSORS
[i2c-slave] tick 0 writes_seen=0 regs={0xa0,0xa1,0xa2,0xa3,...}
...
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
   back the values board A primed into `g_regs[]`.  Note that
   registers 0..1 are read-only in this example (write window
   starts at register 2), so writes to them are dropped.

## Other useful tools

* **Logic analyser.**  Saleae Logic 2 (free decoder for I2C) +
  any cheap clone shows the bus traffic.  Essential when debugging
  bus contention or NACKs.
* **`i2c-scanner`** running on board B with no target attached gives
  a baseline "what's on the bus" reading -- run it before flipping
  on the target to confirm address 0x42 isn't already taken.

## Reference

- [`<alp/i2c_regfile.h>`](../../../include/alp/i2c_regfile.h) -- the
  register-file helper contract this example teaches.
- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) -- the
  "I2C -- target (slave) mode" section documents the raw callback
  contract underneath.
- [`examples/peripheral-io/i2c-scanner/`](../i2c-scanner/) -- master-side discovery.
- [`examples/peripheral-io/i2c-master/`](../i2c-master/) -- master-side known-address read.
- Zephyr `i2c_target_register` -- the upstream API the Zephyr
  backend dispatches through.
