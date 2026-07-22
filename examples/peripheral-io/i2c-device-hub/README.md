# i2c-device-hub

Functional read-out of **every populated device on the EVK
sensor/power I2C bus**, each through its real chip driver -- not a
raw register poke.  This is the "prove the whole board is usable"
demo: one bus, many ICs, each brought up and read for a real value.

Contrasts with the two smaller I2C examples:
[`i2c-scanner`](../i2c-scanner/) *probes* every 7-bit address for
ACKs without knowing what's behind them;
[`i2c-master`](../i2c-master/) *reads* a single known sensor.  This
example drives the full populated set and reports a per-device
verdict plus a final `RESULT` tally.

## Devices exercised

Addresses come from `<alp/boards/alp_e1m_evk.h>`; the populated set
matches the `e1m-evk` board preset.

| Device      | Role              | Addr        | What's read                                |
|-------------|-------------------|-------------|--------------------------------------------|
| ICM-42670   | IMU               | 0x69        | WHO_AM_I + a live accel sample             |
| BMI323      | IMU               | 0x68        | CHIP_ID + a live accel sample              |
| BMP581      | Barometer         | 0x47        | CHIP_ID + a raw pressure/temperature sample |
| INA236 x6   | Rail monitors     | per rail    | Bus voltage (mV) + current (uA) per rail   |
| TAS2563 x2  | I2S smart-amps    | 0x4d / 0x4e | Revision + ACTIVE-mode config readback     |
| TCA6408A    | I/O expander      | 0x20 (TCAL9538 @0x72 alt) | Config + input port          |
| 24C128      | EEPROM            | 0x50        | First 16 bytes                             |

Each device is independent: a missing / DNP part is reported and
skipped, never fatal.  The final `RESULT` line states how many of
the attempted devices answered (`PASS` when all did, `PARTIAL`
otherwise).

Note the pre-respin EVK batch quirk the source documents: on those
boards the ICM-42670 and BMI323 both strap to 0x69 and collide, so
both IMU rows fail until the respin.

## What this shows

* `alp_i2c_open()` -- open `BOARD_I2C_SENSORS` at 100 kHz.
* Seven chip-driver bring-up patterns on one shared bus:
  `icm42670_*`, `bmi323_*`, `bmp581_*`, `ina236_*`, `tas2563_*`,
  `tcal9538_*` (drives either PCA9538-class expander),
  `eeprom_24c128_*`.
* Graceful per-device degradation: absent parts print a diagnostic
  with `alp_last_error()` / the failing `alp_status_t` and the run
  continues.
* `alp_i2c_write_read()` used directly for the small raw readbacks
  (amp MODE_CTRL, expander config/input).

## Build

```bash
# Standalone, native_sim (emul I2C; every device reports absent):
west build -b native_sim/native/64 examples/peripheral-io/i2c-device-hub \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd) \
       -DCONFIG_COMPILER_OPT='"-DALP_BOARD_E1M_EVK"'
west build -t run

# On real silicon: board.yaml targets the E1M-AEN801 (E8) SoM on
# the e1m-evk preset, M55-HE core -- build via the SDK wrapper:
tan build examples/peripheral-io/i2c-device-hub
```

(The `ALP_BOARD_E1M_EVK` define selects the EVK route table for
`BOARD_I2C_SENSORS`; the twister row in `testcase.yaml` sets it the
same way.)

## Expected output

Real hardware (post-respin EVK, everything populated) prints one
line per device with live values, ending in:

```
[devhub] RESULT PASS: 13/13 devices answered
[devhub] done
```

native_sim (emul I2C, nothing registered on the bus -- the
[`boards/`](boards/) overlay + conf route `alp-i2c0` to a
`zephyr,i2c-emul-controller` with no emul targets attached) -- this
is a silicon example, so the native_sim value is just "the plumbing
compiles, links and exits cleanly":

```
[devhub] open BOARD_I2C_SENSORS @ 100 kHz
[devhub] ICM42670 @0x69 init fail (rc=-5; pre-respin collides w/ BMI323 @0x69)
[devhub] BMI323   @0x68 init fail (rc=-5; pre-respin it's at 0x69)
[devhub] BMP581   @0x47 absent (err=0)
...            # each remaining device likewise reports absent
[devhub] RESULT PARTIAL: 0/13 devices answered
[devhub] done
```

## Troubleshooting

* **A single device row fails on real hardware.**  Run
  [`i2c-scanner`](../i2c-scanner/) to see what actually ACKs, then
  compare against the address table above -- DNP options (TCA6408A
  vs TCAL9538) and the pre-respin IMU address collision are the
  usual suspects.
* **`bus open failed`.**  The `alp-i2c` DT alias for
  `BOARD_I2C_SENSORS` isn't routed on your board target; for
  native_sim check that the shipped
  [`boards/native_sim_native_64.overlay`](boards/native_sim_native_64.overlay)
  (the `alp-i2c0` alias) and `CONFIG_EMUL=y CONFIG_I2C_EMUL=y` (from
  [`boards/native_sim_native_64.conf`](boards/native_sim_native_64.conf))
  are in effect.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) -- I2C surface.
- [`<alp/boards/alp_e1m_evk.h>`](../../../include/alp/boards/alp_e1m_evk.h) -- `EVK_I2C_ADDR_*` map.
- [`include/alp/chips/`](../../../include/alp/chips/) -- the chip-driver APIs used here.
- [`examples/peripheral-io/i2c-scanner/`](../i2c-scanner/) -- discovery companion.
- [`examples/peripheral-io/i2c-master/`](../i2c-master/) -- single-device companion.
