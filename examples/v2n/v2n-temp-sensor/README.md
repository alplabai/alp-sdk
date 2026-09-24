# v2n-temp-sensor

Reads the on-module TMP112 once per second and prints the
temperature in degrees Celsius (with 0.001 °C resolution), from a
Linux/Yocto user-space app on the V2N Cortex-A55 cluster.
Classic V2N starter app -- one I²C bus open, one chip init, a
ten-sample loop.

> RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive
> (`metadata/e1m_modules/v2n/core-ownership.yaml`) -- the CM33 must
> never master it. This app runs on the A55, following the same
> pattern as [`v2n-power-monitor`](../v2n-power-monitor/) (portable
> `<alp/i2c.h>` + a natural-name chip driver, Linux `/dev/i2c-N`
> backend).

## What it shows

* `alp_i2c_open(.bus_id = 8)` -- BRD_I2C handle (Linux `/dev/i2c-8`;
  meta-alp-sdk's `e1m-v2n-som.dtsi` aliases `i2c8 = &i2c8;`).
* `tmp112_init(...)` -- ACK-probe at 7-bit address `0x40`
  (`TMP112_I2C_ADDR_ADDRVAR_GND`; ADD0 strapped to GND on the fitted
  TMP112DIDPWR X2SON-5 package -- maintainer-confirmed 2026-09-24,
  see [`metadata/chips/tmp112.yaml`](../../../metadata/chips/tmp112.yaml)).
* `tmp112_read_temp_milli_c(...)` -- 12-/13-bit conversion read,
  returned as signed milli-degrees.
* Clean shutdown in `_deinit`.

## Build (Yocto SDK)

```sh
# Source the SDK that includes meta-alp-sdk (libalp_sdk.so + libalp_chips.a):
. /opt/poky/<ver>/environment-setup-aarch64-poky-linux

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

Copy `build/v2n-temp-sensor` to the target and run it:

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
