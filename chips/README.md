@page chips_impl_index Chip driver implementations

# chips/ — chip driver implementations

One subdirectory per supported part.  Each chip driver is a thin C
binding around a specific IC's datasheet — IMU, OLED, environment
sensor, image sensor, etc.

## Naming convention

Chip driver symbols use the **chip's natural name** as the prefix
(`lsm6dso_init()`, `ssd1306_clear()`, `ov5640_capture()`).  The
`alp_` prefix is reserved for SDK-owned abstractions
(`alp_i2c_*`, `alp_spi_*`, `alp_display_*`, `alp_iot_*`); chip
drivers are bindings to third-party silicon and do not carry it.

The header path *does* keep `alp/` (`#include <alp/chips/lsm6dso.h>`)
because the headers are SDK-shipped, but the C symbols inside use the
bare chip name.

## Layout per chip

```
chips/<part>/
├── README.md
├── CMakeLists.txt
├── <part>.c            # OS-agnostic core driver
├── <part>_zephyr.c     # Zephyr-specific glue (DT binding, k_mutex)
├── <part>_baremetal.c  # bare-metal glue (uses vendors/<som>/ HAL)
├── <part>_yocto.c      # Linux/userspace glue (sysfs, ioctl)
└── tests/
    └── ...
```

The OS-agnostic core uses `<alp/peripheral.h>` for I²C / SPI / GPIO
and `<alp/iot.h>` etc. for higher-level transports — never the
underlying vendor HAL directly.  That keeps every chip driver
portable across the three OS targets.

## v0.1 roster

- `lsm6dso/` — STMicroelectronics 6-axis IMU (alp-studio block
  `blk_imu_lsm6dso`).
- `ssd1306/` — Solomon Systech monochrome OLED controller
  (alp-studio block `blk_oled_ssd1306`).
- `button_led/` — generic button + LED block helper (alp-studio
  block `blk_button_led`).  Note: this is an SDK-level *block*
  utility rather than a single-IC driver, so it carries the `alp_`
  prefix as `alp_button_led_*`.

Implementations land after sign-off on the v0.1 peripheral wrapper
work.  See [`VERSIONS.md`](../VERSIONS.md) for the full roster.
