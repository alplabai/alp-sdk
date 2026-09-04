<!-- Last verified: 2026-08-05 (Step 4 `tan flash` needs the project path -- bare `tan flash` defaults to the current directory, not the built example). -->

# 01 -- First build: GPIO + LED

> **Status:** the board target this tutorial builds against,
> `alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp`, ships in-tree
> under [`zephyr/boards/alp/`](../../zephyr/boards/alp/) (no external
> repo). If you don't have an E1M-AEN801 SoM on hand, follow along on
> `native_sim/native/64` instead — see
> [`docs/getting-started.md` §7](../getting-started.md#7-targeting-real-silicon).

The canonical "your first Alp SDK build."  This tutorial walks
through compiling `examples/peripheral-io/gpio-button-led/` against the
`alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp` board, flashing it,
and watching the console report a bounded LED blink plus a single
button read.  Time budget: 20 minutes.

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
pins:
  - { e1m: E1M_GPIO_IO4,   macro: EVK_PIN_ENCODER_SW }
  - { e1m: E1M_GPIO_PWM0,  macro: EVK_PIN_LED_RED }
cores:
  m55_hp:
    app: ./src                # os: omitted -- M-cores default to zephyr per topology
    peripherals: [gpio]
```

That's it.  No DT overlay, no hand-rolled Kconfig fragment.  The
loader (`scripts/alp_project.py`) reads `board.yaml` at configure
time and emits a Zephyr-side `alp.conf` that gets layered on top
of `prj.conf` via `EXTRA_CONF_FILE`.

## Step 2 -- Read the source

```c
#include "alp/peripheral.h"
#include "alp/blocks/button_led.h"
#include "alp/board.h"          /* BOARD_PIN_* cross-EVK aliases */

int main(void) {
    (void)alp_init();

    alp_button_led_t bl;
    alp_status_t s = alp_button_led_init(&bl, &(alp_button_led_config_t){
        .button_pin_id     = BOARD_PIN_ENCODER_SW,
        .led_pin_id        = BOARD_PIN_LED_RED,
        .active_low_button = true,
    });
    if (s != ALP_OK) return 1;

    /* Bounded blink -- the init smoke test the twister console
     * harness checks for. */
    for (int i = 0; i < 4; i++) {
        alp_button_led_set(&bl, i & 1);
        alp_delay_ms(50);
    }

    bool pressed = false;
    alp_button_led_is_pressed(&bl, &pressed);
    printf("[gpio] is_pressed -> pressed=%d\n", (int)pressed);

    alp_button_led_deinit(&bl);
}
```

`<alp/board.h>` resolves `BOARD_PIN_ENCODER_SW` / `BOARD_PIN_LED_RED`
to the active EVK's routes at compile time -- `EVK_PIN_ENCODER_SW` /
`EVK_PIN_LED_RED` on the E1M EVK, the `XEVK_PIN_*` equivalents on the
E1M-X EVK -- so the same source builds for either board unchanged.
`<alp/blocks/button_led.h>` is the SDK-level block helper: it opens
and configures the button + LED GPIOs internally via
`<alp/peripheral.h>`, so app code only calls `alp_button_led_init` /
`_set` / `_is_pressed` / `_deinit`.

Key contract every Alp peripheral call follows:

* `alp_<class>_open(...)` returns NULL on failure; the failure
  reason is in `alp_last_error()`.  Block helpers built on top
  (`alp_button_led_init`) surface the same failure as a non-`ALP_OK`
  `alp_status_t` return instead.
* Every operation returns an `alp_status_t` (negative = error).
* Cleanup is `alp_<class>_close(...)` / `alp_<block>_deinit(...)`;
  both are safe to call on a handle that never finished opening.

## Step 3 -- Build

```bash
tan build --project examples/peripheral-io/gpio-button-led
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

west build -b alp_e1m_aen801_m55_hp/ae822fa0e5597ls0/rtss_hp examples/peripheral-io/gpio-button-led -- \
    -DEXTRA_CONF_FILE=/tmp/alp.conf \
    -DEXTRA_ZEPHYR_MODULES=alp-sdk
```

## Step 4 -- Flash + run

```bash
tan flash examples/peripheral-io/gpio-button-led
```

(the `APP_PATH` positional -- `tan flash` bare defaults to the current
directory, so it must name the same project Step 3 built if you're still at
the repo root; `--project examples/peripheral-io/gpio-button-led` is
equivalent.)

Watch the console: the LED blinks four times over ~200 ms (the init
smoke test), then the app samples the button once and reports
whether it was pressed at that instant.  Hold the button down before
flashing to see `pressed=1` on the console.

## What to change next

* Try `som.sku: E1M-AEN301` in `board.yaml` -- the build retargets
  to E3 silicon without source-code edits.  This is the
  intra-family portability promise; see
  [Tutorial 04](04-cross-family-portability.md).
* Edit the `pins:` block in `board.yaml` to rebind
  `EVK_PIN_ENCODER_SW` / `EVK_PIN_LED_RED` to different E1M pads --
  the `e1m:` field takes any `ALP_E1M_*` id valid for the active
  preset (see `metadata/boards/e1m-evk.yaml` for the EVK's full
  `e1m_routes:` inventory).

## See also

* [`examples/peripheral-io/gpio-button-led/`](../../examples/peripheral-io/gpio-button-led/)
* [`docs/board-config-schema.md`](../board-config-schema.md) -- the `board.yaml`
  schema reference.
* [`<alp/peripheral.h>`](../../include/alp/peripheral.h),
  [`<alp/blocks/button_led.h>`](../../include/alp/blocks/button_led.h),
  [`<alp/board.h>`](../../include/alp/board.h) -- API surface this
  example uses.
