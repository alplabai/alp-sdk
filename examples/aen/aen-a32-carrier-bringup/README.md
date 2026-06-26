# aen-a32-carrier-bringup

Bring-up smoke test for the **E1M-EVK** carrier peripherals, run from a
Linux/Yocto user-space app on the **E1M-AEN801 Cortex-A32**. It walks
the portable `alp_*` surface over the Linux userspace ABIs:

- `alp_i2c_*` over `/dev/i2c-N` (i2c-dev): a `0x08..0x77` bus scan (bring-up
  convenience only — a blind 1-byte read-probe can disturb action-on-read /
  auto-increment devices on the shared sensor bus such as INA236 and TAS2563;
  do not use as a safe production probe)
- the `tcal9538` I/O-expander at `0x72` (toggle one output, read the port)
- the IMU at `0x68`/`0x69` (`bmi323` first, `icm42670` fallback — reports
  which is populated via CHIP_ID / WHO_AM_I)
- `alp_gpio_*` over the gpiochip v2 chardev ABI (drive the green LED, read
  the BMI323 INT1 line)

> **EVK-only / demo.** A bring-up utility, not a production path.
> Addresses come from `<alp/boards/alp_e1m_evk.h>`; nothing is invented.

## Build (Yocto SDK)

```sh
. /opt/poky/<ver>/environment-setup-arm-poky-linux-musleabi
cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

The recipe `meta-alp-sdk/recipes-examples/aen-a32-carrier-bringup_0.6.bb`
also bakes the binary into `alif-tiny-image` (`/usr/bin/aen-a32-carrier-bringup`).

## Board-gated values (`TODO(e1m-evk-hw)` in `src/main.c`)

Fill these in on the bench from the live `/dev` enumeration, then rebuild:

| Constant | How to resolve |
|---|---|
| `AEN_SENSOR_I2C_ADAPTER` | `i2cdetect -l` → the `/dev/i2c-N` for Alif I2C2 |
| `AEN_PIN_LED_GREEN` | `gpioinfo` → `(gpiochip << 16) \| line` for an output pad |
| `AEN_PIN_BMI323_INT` | `gpioinfo` → packed pin_id for the BMI323 INT1 pad |

> The EVK's RGB LED is PWM-driven (`EVK_PWM_LED_*`), so there may be no
> GPIO-reachable "green LED" line. For the `alp_gpio_*` write exercise,
> substitute any available SoC GPIO output the bench can observe.

> **I/O-expander variant:** the EVK's main I/O-expander can be assembled as
> the TCA6408A at `0x20` instead of the TCAL9538 at `0x72`; on such a board
> the expander step reports `[FAIL]` and the bus scan shows `0x20` — use
> `EVK_I2C_ADDR_TCA6408A_MAIN` if so.

Until then the I2C steps run against the placeholder adapter and the GPIO
steps report `[FAIL]` with the constant to set — by design, not a defect.
