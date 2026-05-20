@page blocks_impl_index Block helper implementations

# blocks/ — SDK-level block helper implementations

One subdirectory per block helper.  A **block** in the ALP SDK is an
SDK-owned abstraction that orchestrates two or more lower-level
peripherals (or a peripheral plus a portable transport) into a
single, reusable unit -- and is therefore *not* a binding to a
specific third-party IC.

## Block vs. chip

| Aspect             | `chips/<part>/`                            | `blocks/<name>/` (here)                         |
|--------------------|--------------------------------------------|--------------------------------------------------|
| Bound to           | One datasheet (LSM6DSO, SSD1306, …)        | A *pattern* over peripherals (button+LED, PDM)   |
| Symbol prefix      | The chip's natural name (`lsm6dso_init()`) | `alp_<block>_*` -- it's an SDK abstraction       |
| Public header path | `<alp/chips/<part>.h>`                     | `<alp/blocks/<name>.h>`                          |
| Kconfig symbol     | `CONFIG_ALP_SDK_CHIP_<NAME>`               | `CONFIG_ALP_SDK_BLOCK_<NAME>`                    |
| Swap-friendly      | No -- replacing the IC means a new driver  | Yes -- any compliant peripheral plugs in         |

The naming-convention split (`alp_` vs natural-name) is documented
in the maintainer memory note `[[chip-driver-naming]]`; this
directory plus `<alp/blocks/>` is the location side of the same
boundary.

## v0.6 roster

- `button_led/` -- generic button + LED helper (alp-studio block
  `blk_button_led`).  Wraps `alp_gpio_*` from `<alp/peripheral.h>`.
- `pdm_mic/` -- generic PDM-microphone surface (alp-studio block
  `blk_pdm_mic`).  v0.2 declares the shape; the underlying
  `alp_i2s_*` peripheral abstraction lands alongside `<alp/audio.h>`.

Both live behind a `CONFIG_ALP_SDK_BLOCK_<NAME>` Kconfig flag and
get auto-enabled when the matching slug appears in the resolved
board's `populated:` block (either inline in the project's
`board.yaml` or pulled in via `preset:`).
