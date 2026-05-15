# 01 -- First build: GPIO + LED

The canonical "your first ALP-SDK build."  This tutorial walks
through compiling `examples/gpio-button-led/` against the
`alp_e1m_evk_aen` board, flashing it, and watching a button toggle
an LED.  Time budget: 20 minutes.

## Prerequisites

* A working Zephyr workspace with this repo loaded as a module via
  `west.yml` (see [`docs/getting-started.md`](../getting-started.md)).
* An E1M-AEN family SoM mounted on an E1M-EVK carrier (any AEN
  variant -- E3..E8 -- works identically for this tutorial).
* A SWD probe attached to the EVK's JTAG header.

## Step 1 -- Inspect the example

```
examples/gpio-button-led/
├── CMakeLists.txt    # invokes scripts/alp_project.py + west
├── prj.conf          # mostly empty -- features come from board.yaml
├── board.yaml        # SoM SKU + carrier + OS + peripherals
├── src/
│   └── main.c        # the application code
```

The whole "what does this app target?" surface lives in
`board.yaml`:

```yaml
schema_version: 2
som:
  sku: E1M-AEN701
carrier:
  name: E1M-EVK
cores:
  m55_hp:
    os: zephyr
    app: ./src
    peripherals: [gpio]
```

That's it.  No DT overlay, no hand-rolled Kconfig fragment.  The
loader (`scripts/alp_project.py`) reads `board.yaml` at configure
time and emits a Zephyr-side `alp.conf` that gets layered on top
of `prj.conf` via `OVERLAY_CONFIG`.

## Step 2 -- Read the source

```c
#include "alp/peripheral.h"

int main(void) {
    alp_gpio_t *button = alp_gpio_open(/*pin_id*/ 0);
    alp_gpio_t *led    = alp_gpio_open(/*pin_id*/ 1);

    alp_gpio_configure(button, ALP_GPIO_INPUT,  ALP_GPIO_PULL_UP);
    alp_gpio_configure(led,    ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE);

    while (1) {
        bool pressed;
        alp_gpio_read(button, &pressed);
        alp_gpio_write(led, !pressed);   /* active-low button */
        k_msleep(10);
    }
}
```

Key contract every ALP peripheral call follows:

* `alp_<class>_open(...)` returns NULL on failure; the failure
  reason is in `alp_last_error()`.
* Every operation returns an `alp_status_t` (negative = error).
* Cleanup is `alp_<class>_close(...)`; idempotent on NULL.

## Step 3 -- Build

```bash
west alp-build -b alp_e1m_evk_aen examples/gpio-button-led
```

If your west workspace doesn't recognise `alp-build`, that's the
SDK's pre-flight wrapper.  Fallback:

```bash
python3 alp-sdk/scripts/alp_project.py \
    --input examples/gpio-button-led/board.yaml \
    --emit zephyr-conf > /tmp/alp.conf

west build -b alp_e1m_evk_aen examples/gpio-button-led -- \
    -DOVERLAY_CONFIG=/tmp/alp.conf \
    -DEXTRA_ZEPHYR_MODULES=alp-sdk
```

## Step 4 -- Flash + run

```bash
west flash
```

Hold the button on the EVK -- the LED should light.  Release --
LED goes dark.

## What to change next

* Try `som.sku: E1M-AEN301` in `board.yaml` -- the build retargets
  to E3 silicon without source-code edits.  This is the
  cross-family portability promise; see
  [Tutorial 04](04-cross-family-portability.md).
* Swap `pin_id: 0/1` for the carrier's actual pin ids declared in
  `metadata/carriers/E1M-EVK/board.yaml`.

## See also

* [`examples/gpio-button-led/`](../../examples/gpio-button-led/)
* [`docs/board-config.md`](../board-config.md) -- the `board.yaml`
  schema reference.
* [`<alp/peripheral.h>`](../../include/alp/peripheral.h) -- API
  surface this example uses.
