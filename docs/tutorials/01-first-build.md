<!-- Last verified: 2026-07-17 (real board target, in-tree). -->

# 01 -- First build: GPIO + LED

> **Status:** the board target this tutorial builds against,
> `alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he`, ships in-tree
> under [`zephyr/boards/alp/`](../../zephyr/boards/alp/) (no external
> repo). If you don't have an E1M-AEN801 SoM on hand, follow along on
> `native_sim/native/64` instead — see
> [`docs/getting-started.md` §7](../getting-started.md#7-targeting-real-silicon).

The canonical "your first Alp SDK build."  This tutorial walks
through compiling `examples/peripheral-io/gpio-button-led/` against the
`alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he` board, flashing it,
and watching a button toggle an LED.  Time budget: 20 minutes.

## Prerequisites

* A working Zephyr workspace with this repo loaded as a module via
  `west.yml` (see [`docs/getting-started.md`](../getting-started.md)).
* An E1M-AEN family SoM mounted on an E1M-EVK board (any AEN
  variant -- E3..E8 -- works identically for this tutorial).
* A SWD probe attached to the EVK's JTAG header.

## Step 1 -- Inspect the example

```
examples/peripheral-io/gpio-button-led/
├── CMakeLists.txt    # invokes scripts/alp_project.py + west
├── prj.conf          # mostly empty -- features come from board.yaml
├── board.yaml        # SoM SKU + board + OS + peripherals
├── src/
│   └── main.c        # the application code
```

The whole "what does this app target?" surface lives in
`board.yaml`:

```yaml
som:
  sku: E1M-AEN801
preset: e1m-evk
cores:
  m55_hp:
    app: ./src                # os: omitted -- M-cores default to zephyr per topology
    peripherals: [gpio]
```

That's it.  No DT overlay, no hand-rolled Kconfig fragment.  The
loader (`scripts/alp_project.py`) reads `board.yaml` at configure
time and emits a Zephyr-side `alp.conf` that gets layered on top
of `prj.conf` via `OVERLAY_CONFIG`.

## Step 2 -- Read the source

```c
#include "alp/peripheral.h"
#include "alp/boards/alp_e1m_evk_routes.h"   /* EVK_PIN_* board macros */

int main(void) {
    alp_gpio_t *button = alp_gpio_open(EVK_PIN_ENCODER_SW);
    alp_gpio_t *led    = alp_gpio_open(EVK_PIN_LED_RED);

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

Key contract every Alp peripheral call follows:

* `alp_<class>_open(...)` returns NULL on failure; the failure
  reason is in `alp_last_error()`.
* Every operation returns an `alp_status_t` (negative = error).
* Cleanup is `alp_<class>_close(...)`; idempotent on NULL.

## Step 3 -- Build

```bash
tan --project examples/peripheral-io/gpio-button-led build
```

`tan` is the standalone build executor (ADR
[0020](../adr/0020-sdk-owns-build-execution.md)); alp-sdk itself only
emits the plan.  If you don't have `tan` installed
([`alplabai/tan-cli`](https://github.com/alplabai/tan-cli)), fall back
to driving `alp_project.py` + `west build` directly:

```bash
python3 alp-sdk/scripts/alp_project.py \
    --input examples/peripheral-io/gpio-button-led/board.yaml \
    --emit zephyr-conf > /tmp/alp.conf

west build -b alp_e1m_aen801_m55_he/ae822fa0e5597ls0/rtss_he examples/peripheral-io/gpio-button-led -- \
    -DOVERLAY_CONFIG=/tmp/alp.conf \
    -DEXTRA_ZEPHYR_MODULES=alp-sdk
```

## Step 4 -- Flash + run

```bash
tan flash
```

Hold the button on the EVK -- the LED should light.  Release --
LED goes dark.

## What to change next

* Try `som.sku: E1M-AEN301` in `board.yaml` -- the build retargets
  to E3 silicon without source-code edits.  This is the
  intra-family portability promise; see
  [Tutorial 04](04-cross-family-portability.md).
* Swap `pin_id: 0/1` for the board's actual pin ids declared in
  `metadata/boards/E1M-EVK/board.yaml`.

## See also

* [`examples/peripheral-io/gpio-button-led/`](../../examples/peripheral-io/gpio-button-led/)
* [`docs/board-config-schema.md`](../board-config-schema.md) -- the `board.yaml`
  schema reference.
* [`<alp/peripheral.h>`](../../include/alp/peripheral.h) -- API
  surface this example uses.
