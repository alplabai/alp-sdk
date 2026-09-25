# v2n-brd-i2c-bringup

Patch-day diagnostic for the V2N SoM's BRD_I2C management bus
(Renesas RIIC8), run from a Linux/Yocto user-space app on the V2N
Cortex-A55 cluster. Scans the bus, distinguishes a bus-level
electrical fault (line held low / missing pull-ups / wrong pinmux)
from per-device failures, then probes every populated IC read-only
and prints a PASS/FAIL/SKIP table.

> RIIC8/BRD_I2C is Cortex-A55/Linux-exclusive in `a55_boot` mode
> (`metadata/e1m_modules/v2n/core-ownership.yaml`) -- the CM33 masters it
> only transiently during its own `cm33_boot` rail sequence
> (`examples/v2n/v2n-cm33-deepx-rail`), never here. This app runs on the A55, following the same
> pattern as [`v2n-power-monitor`](../v2n-power-monitor/) (portable
> `<alp/i2c.h>` + natural-name chip drivers, Linux `/dev/i2c-8`
> backend).

Probed devices (addresses per `metadata/e1m_modules/E1M-V2N101.yaml`):
DA9292 (0x1E, read-only -- U-Boot owns writing it), ACT88760
(0x25+0x26), OPTIGA Trust M (0x30), TMP112 (0x40,
maintainer-confirmed 2026-09-24), TPS628640 (0x4D, assembly option ->
SKIP when absent), RV-3028-C7 (0x52, also Linux kernel-bound as
`/dev/rtc0`), 5L35023B (0x69, maintainer-confirmed 2026-09-24), and
the GD32G553 supervisor over its I2C bridge transport (0x70, also
Linux kernel-bound as a GPIO expander).

**Known limitation on a running target:** 0x52 (RTC) and 0x70 (GD32
bridge) already have real Linux kernel drivers bound
(`rtc-rv3028`, `alplab,gd32-bridge-gpio` -- see meta-alp-sdk's
`e1m-v2n-som.dtsi`). A userspace i2c-dev transaction to an address
the kernel already owns fails with EBUSY, but only via the I2C_SLAVE
ioctl -- a chip driver's own protocol can use the I2C_RDWR ioctl
instead, which does not see that ownership. Both probes therefore
front-load a plain I2C_SLAVE-based address read before ever touching
the chip's own protocol, and report SKIP (kernel-owned) for those two
rows instead of FAIL -- the RTC row then reads the live time through
`/dev/rtc0`.

The example never writes a PMIC voltage, enable, or control
register (DA9292 CH2 is U-Boot's alone -- see
`meta-alp-sdk/recipes-bsp/u-boot/u-boot/
0004-rzv2n-dev-ALP-E1M-DEEPX-rail-bringup.patch`).

## Build (Yocto SDK)

```sh
# Source the SDK that includes meta-alp-sdk (libalp_sdk.so + libalp_chips.a):
. /opt/poky/<ver>/environment-setup-aarch64-poky-linux

cmake -B build -S . -DCMAKE_TOOLCHAIN_FILE=$OECORE_NATIVE_SYSROOT/usr/share/cmake/OEToolchainConfig.cmake
cmake --build build
```

Copy `build/v2n-brd-i2c-bringup` to the target and run it.
