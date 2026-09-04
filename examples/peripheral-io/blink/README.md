# blink

Toggle an LED.  The canonical first program -- if you're new to the
Alp SDK, start here before anything else in this repo.

## The interesting part: there's no `led0`

E1M is a bare compute module, not a board.  Neither E1M-AEN801 nor
any other SoM in `metadata/e1m_modules/` carries an LED key -- the
LED is a fact about whichever **carrier board** it's plugged into, so
it's declared in this example's `board.yaml`, not on the SoM.

Both Alp Lab EVKs happen to put their only user-visible LED on an
RGB cluster wired to PWM-capable pads, not a dedicated single-colour
GPIO LED.  E1M makes GPIO a universal *secondary* function on every
digital pad (e1m-spec STANDARD.md "GPIO secondary"): a pad whose
default function is PWM can still be driven as a plain digital
output by opening its parallel `ALP_E1M_GPIO_<class><N>` index
instead of its peripheral id.  This demo claims the RGB cluster's
**red** channel that way:

- E1M EVK: `ALP_E1M_GPIO_PWM0` (the PWM0 pad as GPIO -- bench-measured
  2026-07-28; see `metadata/boards/e1m-evk.yaml`'s `e1m_routes:` comment
  for why this isn't PWM3)
- E1M-X EVK: `ALP_E1M_X_GPIO_PWM5` (the PWM5 pad as GPIO)

Never hold both the GPIO and PWM claim on the same pad at once --
it's the same silicon underneath (see
`examples/peripheral-io/pwm-led-fade` for the PWM-side claim on the
same pad).

## Why `BOARD_PIN_LED_RED`, not a raw pin number

The source opens `BOARD_PIN_LED_RED` from `<alp/board.h>`, never
`ALP_E1M_GPIO_PWM3` / `ALP_E1M_X_GPIO_PWM5` directly.  The facade
picks the active board's generated routes header at compile time
from the `ALP_BOARD_<SLUG>` define the build emits from this
example's `board.yaml` `preset:`, so the exact same `src/main.c`
builds unchanged for either EVK -- copy this example, change nothing
but `board.yaml`, and it targets the other EVK.

Want green or blue?  Both EVK route tables declare the full RGB set
(`EVK_PIN_LED_GREEN` / `EVK_PIN_LED_BLUE` in
[`alp_e1m_evk_routes.h`](../../../include/alp/boards/alp_e1m_evk_routes.h),
`XEVK_PIN_LED_GREEN` / `XEVK_PIN_LED_BLUE` in
[`alp_e1m_x_evk_routes.h`](../../../include/alp/boards/alp_e1m_x_evk_routes.h))
-- and both alias all three channels, so `BOARD_PIN_LED_GREEN` and
`BOARD_PIN_LED_BLUE` reach through `<alp/board.h>` exactly like
`BOARD_PIN_LED_RED`.  Swap the macro in `src/main.c`; no
`ALP_BOARD_E1M_EVK` / `ALP_BOARD_E1M_X_EVK` guard needed.

## What this shows

- `alp_gpio_open()` / `alp_gpio_configure()` / `alp_gpio_write()` /
  `alp_gpio_close()` -- the raw `<alp/peripheral.h>` GPIO surface,
  no block helper.
- `alp_gpio_open()` returning `NULL` on failure (frozen at v0.1, no
  room for a status return) and `alp_last_error()` to learn why.
- The pin-as-GPIO secondary function (driving a PWM pad as GPIO).
- The portable `BOARD_*` alias pattern for cross-EVK source.

## Build

```bash
cd examples/peripheral-io/blink
tan build
```

`tan build` takes no positional path -- from elsewhere, point it at
the project explicitly:

```bash
tan build --project examples/peripheral-io/blink
```

`tan` is the SDK's standalone build executor (a separate install --
see [`docs/getting-started.md`](../../../docs/getting-started.md)).
The **first run bootstraps automatically** (fetches its own pinned
tool state); after that it validates `board.yaml`, generates the
build config, and delegates to `west build`.  You still need a
**Zephyr SDK toolchain** on `PATH` / `ZEPHYR_SDK_INSTALL_DIR` for a
real-silicon build -- if `tan build` fails looking for it, that's
this exact gap (tracked as
[tan-cli#160](https://github.com/alplabai/tan-cli/issues/160)); run
`tan doctor` to confirm what's missing before re-running.

To target a different SoM / board, edit `board.yaml` -- nothing else
needs to change.

### native_sim (host, no hardware)

This is the SDK-contributor / CI path, and it is deliberately `west`
rather than `tan`: `native_sim` is a Twister platform, not something
`board.yaml` can select (`os:` is `zephyr`/`yocto`/`baremetal`/`off`),
so `tan build` always targets the real SKU and has no `-b` equivalent.

```bash
west build -b native_sim/native/64 examples/peripheral-io/blink \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd) -DCONFIG_COMPILER_OPT='"-DALP_BOARD_E1M_EVK"'
west build -t run
```

## Expected output

Real hardware (either EVK):

```
[blink] init led=BOARD_PIN_LED_RED
[blink] led=1 status=0
[blink] led=0 status=0
...                        # LED visibly toggles, ~5 times/sec
[blink] done
```

native_sim -- **both** variants run the full open/configure/write
path against an emulated GPIO controller, so the output is the same
shape as hardware minus the photons:

```
[blink] init led=BOARD_PIN_LED_RED
[blink] led=1 status=0
...
[blink] done
```

The pin array in
[`boards/native_sim_native_64.overlay`](boards/native_sim_native_64.overlay)
is indexed by pin id and the two EVKs number their pads differently
(`BOARD_PIN_LED_RED` resolves to `ALP_E1M_GPIO_PWM0` = 26 on the E1M
EVK, `ALP_E1M_X_GPIO_PWM5` = 41 on the E1M-X EVK), so it runs to
index 41 across two emulated controllers -- a `zephyr,gpio-emul` tops
out at 32 pins.

`[blink] done` is the twister harness's success marker and every
failure path prints `[blink] failed: ...` and returns non-zero
instead, so a green test means the LED was actually driven. That is
deliberate: an earlier draft stopped the array at index 29, the
E1M-X variant's `alp_gpio_open()` returned `NULL`, and the test went
green anyway because the failure path also printed `done`.

## Reference

- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) -- GPIO surface.
- [`<alp/board.h>`](../../../include/alp/board.h) -- the cross-EVK `BOARD_*` facade.
- [`<alp/e1m_pinout.h>`](../../../include/alp/e1m_pinout.h) -- the
  `ALP_E1M_GPIO_<class><N>` pin-as-GPIO indices + the positional
  pin-array invariant.
- [`docs/board-config-schema.md`](../../../docs/board-config-schema.md)
  -- the authoritative field reference for `board.yaml`.
- [`examples/peripheral-io/gpio-button-led`](../gpio-button-led/) --
  the next step up: a button + LED pair via the `button_led` block.
- [`examples/peripheral-io/pwm-led-fade`](../pwm-led-fade/) -- the
  same RGB-red pad, driven as real PWM instead of plain GPIO.
