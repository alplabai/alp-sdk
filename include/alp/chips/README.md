@page chips_index Chip drivers index

# include/alp/chips/ — public chip-driver headers

One header per supported chip.  These are the headers that
alp-studio's block init templates `#include`:

```c
#include <alp/chips/lsm6dso.h>
static lsm6dso_t imu;
static int imu_init(void) { return lsm6dso_init(&imu, ...); }
```

## Naming

Header path keeps the `alp/` prefix (it's an SDK-shipped header), but
the symbols inside use the **chip's natural name** — `lsm6dso_t`,
`lsm6dso_init()`.  No `alp_lsm6dso_*`.  See [`chips/README.md`](../../../chips/README.md).

## v0.1 surface (planned)

| Header              | Chip                                  | Block consumer            |
|---------------------|---------------------------------------|---------------------------|
| `lsm6dso.h`         | STMicroelectronics LSM6DSO IMU        | `blk_imu_lsm6dso`         |
| `ssd1306.h`         | Solomon Systech SSD1306 OLED          | `blk_oled_ssd1306`        |

SDK-level *block* helpers (`<alp/blocks/button_led.h>`,
`<alp/blocks/pdm_mic.h>`) live one directory up under
[`include/alp/blocks/`](../blocks/) rather than here; they are
abstractions over multiple peripherals and carry the `alp_*`
prefix (`alp_button_led_*`, `alp_pdm_mic_*`).  See
[`blocks/README.md`](../../../blocks/README.md).

Headers land alongside the implementations under [`chips/`](../../../chips/).
