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
the last row (`5V`).  Its shunt is 100 mΩ.

### INA228 shunt scale (`--ina228-range`)

The INA228 measures the shunt on one of two scales (`CONFIG.ADCRANGE`).  With
the 100 mΩ shunt:

| Option | Shunt full scale | Current full scale | Resolution |
|---|---|---|---|
| `163mv` (board default) | ±163.84 mV | 1.6384 A | 3.125 µA/LSB |
| `40mv` | ±40.96 mV | 0.4096 A | 0.78125 µA/LSB (4x finer) |
| `auto` | starts on `163mv` | | |

```sh
./v2n-power-monitor --ina228-range auto      # or 163mv | 40mv
```

The default comes from the board header (`XEVK_INA228_ADCRANGE_5V`, the wide
range): the +5V input current with the NPU active is not known to stay under
0.4096 A, so the board default is not the narrow range.  The max current the
example passes is the range's shunt full scale (1.6384 A on the wide range,
0.4096 A on the narrow one), derived from the shunt, not a limit of the rail.

`auto` starts on the wide range and decides from each reading's shunt
voltage: below 75 % of the narrow full scale (30.72 mV) it switches to
`40mv`; at or above 95 % of it (38.912 mV), or as soon as the narrow range
is clipped, it switches back to `163mv`.  Between the two it stays where it
is, so a reading near a threshold does not flip the range.  Each switch
prints one line and shows `--` for that one sample.  Switching resets the
INA228's energy and charge accumulators (they were counted at the old scale).
A reading that clips (`ina228_check_over_range()`: the shunt ADC at its
limit, or the `MATHOF` flag) is reported as `over-range` instead of being
printed as a valid number.  The decision logic is `src/range_policy.h`, a
pure function covered by `tests/zephyr/chips/src/test_ina228.c`.

The INA228 answers only on carriers with the I2C bus-pin rework applied.
On a carrier without it the driver reports "not present": the example
prints one line to stderr, shows `--` in the `5V` row and keeps running,
and the loop keeps running (an absent INA228 does not stop it).  Any other
failure is printed with its actual status (for example `ALP_ERR_BUSY` when a
kernel driver already holds the address), never as "not present".

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

Copy `build/v2n-power-monitor` to the target and run it (Ctrl-C to stop).
Illustrative output, not captured from a board; the `5V` row shows `--` as it
does on a carrier without the INA228 bus-pin rework (on a reworked carrier it
shows the live +5V reading instead):

```
rail     bus_V     I_mA       P_mW
  3V3      0.099       0.00        0.0
  1V8      0.002       0.00        0.0
  VCAM3    0.000       0.00        0.0
  5V         --        --         --
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
