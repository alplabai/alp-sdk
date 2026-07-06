# i2c-master

Discrete I2C master that reads a known device at a known
address.  Pattern: open the bus, init the chip driver, loop
reading the sensor every second, close cleanly.

Contrasts with [`examples/peripheral-io/i2c-scanner`](../i2c-scanner/) -- the
scanner *probes* every 7-bit address for ACKs without knowing
what's behind them; this example *reads* a known sensor.

## What this shows

* `alp_i2c_open()` -- open `ALP_E1M_I2C0` at 400 kHz Fast-mode.
* `tmp112_init()` -- chip-driver probe + configure.
* `tmp112_set_rate()` -- tune the conversion cadence.
* `tmp112_read_temp_milli_c()` -- one register read per second.
* Error handling: NACK on probe -> graceful exit with diagnostic.
* `tmp112_deinit()` + `alp_i2c_close()` -- clean shutdown.

## Hardware

The TMP112 +/-0.5 °C temperature sensor is populated on the
BRD_I2C management bus of every member of the AEN, V2N, and
V2N-M1 families per
[`metadata/chips/tmp112.yaml`](../../../metadata/chips/tmp112.yaml).

7-bit address depends on the ADD0 strap:

| ADD0 strap | Address | SoM defaults                              |
|------------|---------|-------------------------------------------|
| GND        | 0x48    | E1M-AEN + E1M-V2N families (this default) |
| V+         | 0x49    | (none today)                              |
| SDA        | 0x4A    | (none today)                              |
| SCL        | 0x4B    | (none today)                              |

All current SoM families strap ADD0 to GND, so `TMP112_ADDR_7BIT`
works unchanged across them (see the `scope:` note in
[`metadata/chips/tmp112.yaml`](../../../metadata/chips/tmp112.yaml),
TMP112 datasheet SBOS473K table 2, or
[`include/alp/chips/tmp112.h`](../../../include/alp/chips/tmp112.h)).

## Build

```bash
# Standalone, native_sim (emul I2C; tmp112_init NACKs cleanly):
west build -b native_sim/native/64 examples/peripheral-io/i2c-master \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run

# On real silicon, point -b at the SoM's Zephyr board target.
# Example for E1M-AEN801:
west build -b alp_e1m_aen801_m55_hp examples/peripheral-io/i2c-master
west flash
```

## Expected output

Real hardware (TMP112 populated, room temperature):

```
[i2c-master] open ALP_E1M_I2C0 @ 400 kHz
[i2c-master] tmp112_init @ 0x48 -> 0 (OK)
[i2c-master] sample 0: 23.625 degC
[i2c-master] sample 1: 23.687 degC
[i2c-master] sample 2: 23.625 degC
[i2c-master] sample 3: 23.687 degC
[i2c-master] sample 4: 23.750 degC
[i2c-master] done
```

native_sim (emul I2C, no TMP112 registered):

```
[i2c-master] open ALP_E1M_I2C0 @ 400 kHz
[i2c-master] tmp112_init @ 0x48 -> -5 (populated? right address?)
[i2c-master] done
```

## Troubleshooting

* **`tmp112_init -> -5`** (ALP_ERR_IO / NACK).  Either the chip
  isn't populated on your board, the address is wrong (see
  table above), or the bus is held low (missing pull-ups, stuck
  slave).  Run `examples/peripheral-io/i2c-scanner` to confirm what ACKs.
* **`alp_i2c_open failed`** (NULL return).  The `alp-i2c0` DT
  alias isn't set -- check your board overlay or, for
  native_sim, that `CONFIG_EMUL=y CONFIG_I2C_EMUL=y` and the
  overlay we ship are picked up.
* **Garbled readings.**  Wrong baud (bitrate) for the bus
  capacitance.  Drop from 400 kHz to 100 kHz to confirm.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) I2C surface.
- [`<alp/chips/tmp112.h>`](../../../include/alp/chips/tmp112.h) -- driver API.
- [`examples/peripheral-io/i2c-scanner/`](../i2c-scanner/) -- discovery companion.
- [`examples/peripheral-io/i2c-slave/`](../i2c-slave/) -- slave-mode companion (API gap; see notes).
- TMP112 datasheet (TI SBOS473K).
