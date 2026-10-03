# v2n-soc-temperature

Prints the SoC die (junction) temperature once per second from a
Linux/Yocto user-space app on the V2N Cortex-A55 cluster, through the
portable `alp_temperature_read_soc_milli_c()`.

This is **not** the on-module ambient sensor. That one is
`alp_temperature_read_milli_c()` (a TMP112 on the SoM; see
[`v2n-temp-sensor`](../v2n-temp-sensor/)) and the two are separate API
entries on purpose (issue #2066).

## What it shows

* `alp_temperature_read_soc_milli_c(&milli_c)` -- hottest die sensor,
  signed milli-degrees Celsius.
* `ALP_ERR_NOSUPPORT` handling -- returned when the build has no SoC
  thermal source (Zephyr / baremetal, or a kernel with no matching zone).
* Sign-safe printing of a milli-degree value.

## How it works

The Yocto backend (`src/yocto/temperature_yocto.c`) reads
`/sys/class/thermal/thermal_zoneN/temp` for every zone whose `type`
starts with `cpu-thermal` (the RZ/V2N's two TSU units, `cpu-thermal0`
and `cpu-thermal1` in the vendor device tree). Zones are chosen by
type, never by index, because the index follows probe order. The kernel
keeps ownership of the TSU.

## Build (Yocto SDK)

```sh
. /opt/poky/<ver>/environment-setup-aarch64-poky-linux

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

Copy `build/v2n-soc-temperature` to the target and run it. Output has
the shape:

```
[soc-temp] v2n-soc-temperature
[soc-temp] sample 0: <whole>.<milli> degC
...
[soc-temp] done
```

Not yet run on hardware.
