# i2c-master

Discrete I2C master that reads a known device at a known
address.  Pattern: open the bus, init the chip driver, loop
reading the sensor every second, close cleanly.

Contrasts with [`examples/peripheral-io/i2c-scanner`](../i2c-scanner/) -- the
scanner *probes* every 7-bit address for ACKs without knowing
what's behind them; this example *reads* a known sensor.

## What this shows

* `alp_i2c_open()` -- open `BOARD_I2C_SENSORS` at 400 kHz Fast-mode.
  The `<alp/board.h>` alias resolves to `ALP_E1M_I2C0` on the E1M
  EVK and `ALP_E1M_X_I2C0` on the E1M-X EVK.
* `bmp581_init()` -- chip-driver probe (CHIP_ID check).
* `bmp581_set_sampling()` -- configure oversampling, ODR, and power mode.
* `bmp581_read_raw()` + `bmp581_compensate()` -- one pressure+temperature read per second.
* Error handling: NACK on probe -> graceful exit with diagnostic.
* `bmp581_deinit()` + `alp_i2c_close()` -- clean shutdown.

## Hardware

The BMP581 barometer is the on-board sensor on `BOARD_I2C_SENSORS`
on both the E1M EVK (U14) and the E1M-X EVK, per
[`metadata/boards/e1m-evk.yaml`](../../../metadata/boards/e1m-evk.yaml) and
[`metadata/boards/e1m-x-evk.yaml`](../../../metadata/boards/e1m-x-evk.yaml)
`i2c_devices:`.

7-bit address depends on the SDO strap:

| SDO strap | Address | On this bus                          |
|-----------|---------|---------------------------------------|
| VDDIO     | 0x47    | E1M EVK U14 + E1M-X EVK (this default) |
| GND       | 0x46    | (not populated on these EVKs)          |

Both supported EVKs strap SDO to VDDIO, so `BMP581_ADDR_7BIT`
works unchanged across them (see
[`metadata/boards/e1m-evk.yaml`](../../../metadata/boards/e1m-evk.yaml),
BMP581 datasheet BST-BMP581-DS004 s5.6, or
[`include/alp/chips/bmp581.h`](../../../include/alp/chips/bmp581.h)).

> **#1269:** this example used to target the TMP112 temperature sensor,
> but TMP112 lives on **BRD_I2C**, not on `BOARD_I2C_SENSORS` -- opening
> `BOARD_I2C_SENSORS` and probing for TMP112 NACKs on real hardware. On
> the E1M-AEN family BRD_I2C is additionally the slave-only Alif LPI2C0
> (ADR 0017), so the SoC can't master it at all. See
> [`examples/v2n/v2n-temp-sensor`](../../v2n/v2n-temp-sensor/) for the
> V2N-only BRD_I2C/TMP112 pattern instead.

## Build

```bash
# Standalone, native_sim (emul I2C; bmp581_init NACKs cleanly):
west build -b native_sim/native/64 examples/peripheral-io/i2c-master \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN801:
west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/peripheral-io/i2c-master
west flash
```

## Expected output

Real hardware (BMP581 populated, room temperature, sea-level-ish pressure):

```
[i2c-master] open BOARD_I2C_SENSORS @ 400 kHz
[i2c-master] bmp581_init @ 0x47 -> 0 (OK)
[i2c-master] sample 0: 96234 Pa, 23.625 degC
[i2c-master] sample 1: 96231 Pa, 23.687 degC
[i2c-master] sample 2: 96233 Pa, 23.625 degC
[i2c-master] sample 3: 96230 Pa, 23.687 degC
[i2c-master] sample 4: 96232 Pa, 23.750 degC
[i2c-master] done
```

native_sim (emul I2C, no BMP581 registered):

```
[i2c-master] open BOARD_I2C_SENSORS @ 400 kHz
[i2c-master] bmp581_init @ 0x47 -> -5 (populated? right address?)
[i2c-master] done
```

## Troubleshooting

* **`bmp581_init -> -5`** (ALP_ERR_IO / NACK).  Either the chip
  isn't populated on your board, the address is wrong (see
  table above), or the bus is held low (missing pull-ups, stuck
  slave).  Run [`examples/peripheral-io/i2c-scanner`](../i2c-scanner/) to confirm what
  ACKs.
* **`alp_i2c_open failed`** (NULL return).  The `alp-i2c0` DT
  alias isn't set -- check your board overlay or, for
  native_sim, that `CONFIG_EMUL=y CONFIG_I2C_EMUL=y` and the
  overlay we ship are picked up.
* **Garbled readings.**  Wrong baud (bitrate) for the bus
  capacitance.  Drop from 400 kHz to 100 kHz to confirm.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) I2C surface.
- [`<alp/chips/bmp581.h>`](../../../include/alp/chips/bmp581.h) -- driver API.
- [`examples/peripheral-io/i2c-scanner/`](../i2c-scanner/) -- discovery companion.
- [`examples/peripheral-io/i2c-slave/`](../i2c-slave/) -- slave-mode companion, built on the `alp_i2c_target_*` surface.
- [`examples/v2n/v2n-temp-sensor/`](../../v2n/v2n-temp-sensor/) -- TMP112-on-BRD_I2C companion, V2N-only.
- BMP581 datasheet (BST-BMP581-DS004, rev 1.13).
