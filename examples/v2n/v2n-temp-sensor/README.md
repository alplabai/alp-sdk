# v2n-temp-sensor

Reads the on-module TMP112 once per second and prints the
temperature in degrees Celsius (with 0.001 °C resolution).
Classic V2N starter app -- one I²C bus open, one chip init, a
ten-sample loop.

## What it shows

* `alp_i2c_open(...)` -- BRD_I2C handle.
* `tmp112_init(...)` -- ACK-probe at 7-bit address `0x40`.
* `tmp112_read_temp_milli_c(...)` -- 12-/13-bit conversion read,
  returned as signed milli-degrees.
* Clean shutdown in `_deinit`.

## Expected output (real hardware)

```
[temp] v2n-temp-sensor
[temp] sample 0: 24.625 degC
[temp] sample 1: 24.687 degC
...
[temp] sample 9: 25.125 degC
[temp] done
```

## See also

* [`<alp/chips/tmp112.h>`](../../../include/alp/chips/tmp112.h)
  -- driver API.
* TI TMP112 datasheet -- ±0.5 °C accuracy, 12/13-bit resolution,
  0.0625 °C/LSB.
