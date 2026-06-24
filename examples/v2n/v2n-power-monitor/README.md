# v2n-power-monitor

Live per-rail power table from the **E1M-X EVK**'s on-board INA236
current/voltage monitors, read from a Linux/Yocto user-space app on
the V2N Cortex-A55.

It opens the on-board sensor I2C bus (`XEVK_I2C_BUS_SENSORS` =
`E1M_X_I2C0`, i.e. `/dev/i2c-0`), calibrates one `ina236` driver
instance per rail using the shunt values from
`<alp/boards/alp_e1m_x_evk.h>`, and prints bus voltage / current /
power once a second.

## Rails

| Rail  | INA236 | Addr | Shunt |
|-------|--------|------|-------|
| 3V3   | U21    | 0x40 | 20 mΩ |
| 1V8   | U31    | 0x41 | 20 mΩ |
| VCAM2 | U32    | 0x48 | 50 mΩ |
| VCAM3 | U34    | 0x49 | 50 mΩ |
| 5V    | U30    | 0x4A | 20 mΩ |

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
  VCAM2    0.000       0.00        0.0
  VCAM3    0.000       0.00        0.0
  5V       4.884     740.00     3614.0
```

## Known board notes (current EVK revision)

To be fixed on the next board revision (the app still runs; affected
rails just read low):

- **3V3 / 1V8** read ~0 V on the bus-voltage register (VBUS-sense
  wiring); their shunt/current path is unaffected.
- **VCAM2 / VCAM3** read ~0 — camera rails are off unless a camera
  is powered.
- Two extra addresses (`0x42` / `0x43`) also respond as INA236 but
  are not in the schematic BOM; not used by this app.
