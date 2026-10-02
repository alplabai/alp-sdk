# v2n-power-monitor

Live per-rail power table from the **E1M-X EVK**'s on-board INA236
current/voltage monitors and its INA228 +5V input monitor, read from a
Linux/Yocto user-space app on the V2N Cortex-A55.

It opens the on-board sensor I2C bus (`XEVK_I2C_BUS_SENSORS` =
`ALP_E1M_X_I2C0`, i.e. `/dev/i2c-0`), calibrates one `ina236` driver
instance per rail using the shunt values from
`<alp/boards/alp_e1m_x_evk.h>`, and prints bus voltage / current /
power once a second.

## Rails

| Rail  | INA236 | Addr | Shunt |
|-------|--------|------|-------|
| 3V3   | U21    | 0x40 | 20 mΩ |
| 1V8   | U31    | 0x41 | 20 mΩ |
| VCAM3 | U34    | 0x49 | 50 mΩ |

The +5V input monitor (U30) is an INA228 at `0x42`, a different
register map, so it is read through the `ina228` driver and printed as
the last row (`5V`).  Its shunt is 100 mΩ; the example passes the
ADCRANGE = 0 shunt full scale (163.84 mV / 100 mΩ = 1.6384 A) as the max
current, a value derived from the shunt, not a limit of the rail.

The INA228 answers only on carriers with the I2C bus-pin rework applied.
On a carrier without it the driver reports "not present": the example
prints one line to stderr, shows `--` in the `5V` row and keeps running,
and the exit status is unaffected.

> **EVK-only / demo.** The INA236 monitors exist only on the EVK
> carriers; production E1M-X SoMs do not carry them. This is a
> bring-up / demo utility, not a production telemetry path.

## Build (Yocto SDK)

```sh
# Source the SDK that includes meta-alp-sdk (libalp_sdk.so + libalp_chips.a):
. /opt/poky/<ver>/environment-setup-aarch64-poky-linux

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

Copy `build/v2n-power-monitor` to the target and run it (Ctrl-C to stop):

```
rail     bus_V     I_mA       P_mW
  3V3      0.099       0.00        0.0
  1V8      0.002       0.00        0.0
  VCAM3    0.000       0.00        0.0
  5V       0.000       0.00        0.0
```

## Known board notes (current EVK revision)

To be fixed on the next board revision (the app still runs; affected
rails just read low):

- **3V3 / 1V8** read ~0 V on the bus-voltage register (VBUS-sense
  wiring); their shunt/current path is unaffected.
- **VCAM3** reads ~0 — the camera rail is off unless a camera
  is powered.
- There is no VCAM2 monitor: `0x48` on this bus is the TAS2563 amplifiers'
  shared address, so the table has no VCAM2 row.
