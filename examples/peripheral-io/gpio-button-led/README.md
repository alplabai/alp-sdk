# gpio-button-led

Read a button and toggle an LED -- both as plain GPIO, through the
`<alp/blocks/button_led.h>` block helper.

This example is also the **canonical demonstration of the board.yaml
+ loader workflow**: every `CONFIG_*` knob the build needs comes from
`board.yaml` -> `scripts/alp_project.py` -> `build/generated/alp.conf`.

## The interesting part: pin-as-GPIO

E1M makes **GPIO a universal secondary on every digital pad**
(e1m-spec STANDARD.md "GPIO secondary").  A pad whose default function
is PWM/ADC/etc. can be claimed as a digital GPIO instead -- you open it
with its parallel `ALP_E1M_GPIO_<class><N>` index, and the board DTS muxes
the pad to GPIO.

The E1M-EVK has no dedicated GPIO LED; its only user LEDs are the RGB
cluster on the PWM pads.  So this demo:

- reads the **encoder push switch** on `ALP_E1M_GPIO_IO4` (active-low) as
  the button, and
- drives the **RGB-red pad** (default function PWM3) as a digital GPIO
  via `ALP_E1M_GPIO_PWM3` (index 29) as the LED.

The portable rule (`e1m_pinout.h` "Pin-as-GPIO fallback"): open a pad's
analog/timer function with its peripheral id (`ALP_E1M_PWM3` ->
`alp_pwm_open`), or claim it as a GPIO with its `ALP_E1M_GPIO_<class><N>`
index (`ALP_E1M_GPIO_PWM3` -> `alp_gpio_open`).  Never hold both against
the same pad -- the silicon is shared.

> The `alp,pin-array` is **positional**: `alp_gpio_open(pin_id)`
> indexes it by the E1M GPIO index, so a board's array lists pads in
> the `e1m_pinout.h` canonical order (IO0..25 = 0..25, PWM0..7 =
> 26..33, ...).  See `boards/native_sim_native_64.overlay` for the
> host-emulated version this example builds against.

## What this shows

- The `button_led` block: `alp_button_led_init` / `_set` /
  `_is_pressed` / `_deinit` over a GPIO button + GPIO LED pair.
- The pin-as-GPIO secondary function (driving a PWM pad as a GPIO).
- The **single-source-of-truth** project config: `board.yaml` drives
  Kconfig generation; `prj.conf` carries no hand-set CONFIG_* knobs.

## Build

```bash
west build -b native_sim/native/64 examples/peripheral-io/gpio-button-led \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

To target a different SoM / board, edit `board.yaml` -- nothing else
needs to change.

## Reference

- [`<alp/blocks/button_led.h>`](../../../include/alp/blocks/button_led.h)
- [`<alp/peripheral.h>`](../../../include/alp/peripheral.h) GPIO surface
- [`<alp/e1m_pinout.h>`](../../../include/alp/e1m_pinout.h) -- the
  `ALP_E1M_GPIO_<class><N>` pin-as-GPIO indices + the positional
  pin-array invariant.
- [`docs/board-config.md`](../../../docs/board-config.md) -- the
  authoritative reference for `board.yaml`.
- [`scripts/alp_project.py`](../../../scripts/alp_project.py) -- the
  loader this example's `CMakeLists.txt` invokes.
