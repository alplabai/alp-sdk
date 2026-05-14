# gpio-button-led

Per-peripheral example wrapping `<alp/peripheral.h>` GPIO via the
`<alp/chips/button_led.h>` block helper.  Polls a button and
toggles an LED.

This example is also the **canonical demonstration of the
board.yaml + loader workflow**: every `CONFIG_*` knob the build
needs comes from `board.yaml` -> `scripts/alp_project.py` ->
`build/generated/alp.conf`, with `prj.conf` reduced to a single
`rsource` line.

## What this shows

- The portable button + LED block pattern: studio-resolved
  `pin_id` integers (`E1M_GPIO_IO0` / `E1M_GPIO_IO1`) feed
  `alp_button_led_init`.
- Active-low button convention.
- Press detection via polling (interrupt path is the helper's
  `alp_button_led_set_press_callback`).
- The **single-source-of-truth** project config: `board.yaml`
  drives Kconfig generation; `prj.conf` carries no hand-set
  CONFIG_* knobs.

## Generated config from board.yaml

```bash
# Run from the example directory.  The path to the SDK is resolved
# via the ALP_SDK_ROOT env var (or, when running from the in-tree
# checkout, by the example's CMakeLists.txt).
python3 ../../scripts/alp_project.py \
    --input  board.yaml \
    --emit   zephyr-conf \
    --output build/generated/alp.conf
```

The matching `prj.conf` is **empty** -- every `CONFIG_*` knob comes
from the generated `alp.conf` via Zephyr's `OVERLAY_CONFIG`
mechanism, wired up at CMake-configure time by the example's
`CMakeLists.txt` (see the `execute_process` + `list(APPEND
OVERLAY_CONFIG ...)` near the top of the file).  A plain
`west build` works without a separate generate step.

(An earlier iteration of this example used `rsource
"build/generated/alp.conf"` in `prj.conf`; that's invalid -- `rsource`
is a Kconfig-source directive, not a `.conf` directive, and Zephyr
rejects it with a malformed-line warning.  The `OVERLAY_CONFIG`
path is the right one and the example uses it now.)

What the generated `alp.conf` looks like for this example (subset
-- the EVK preset enables the carrier's stock chip set in addition
to `button_led`):

```kconfig
# ALP SDK + Zephyr baseline
CONFIG_ALP_SDK=y
CONFIG_LOG=y
CONFIG_PRINTK=y
CONFIG_THREAD_LOCAL_STORAGE=y
CONFIG_LOG_DEFAULT_LEVEL=3

# SoM silicon (alif:ensemble:e7 via E1M-AEN701)
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y

# Carrier chip drivers (E1M-EVK)
CONFIG_ALP_SDK_CHIP_BUTTON_LED=y
...

# Zephyr subsystems pulled in by the enabled chip drivers
CONFIG_GPIO=y
CONFIG_I2C=y
```

Edit `board.yaml`, rebuild, and the regenerated `alp.conf` flows
into the next build automatically.

## Build

```bash
west build -b native_sim/native/64 examples/gpio-button-led \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

To target a different SoM / carrier, edit `board.yaml` -- nothing
else needs to change.

## Reference

- [`<alp/chips/button_led.h>`](../../include/alp/chips/button_led.h)
- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) GPIO surface
- [`docs/board-config.md`](../../docs/board-config.md) -- the
  authoritative reference for `board.yaml`.
- [`scripts/alp_project.py`](../../scripts/alp_project.py) -- the
  loader this example's `CMakeLists.txt` invokes.
