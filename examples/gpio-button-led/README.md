# gpio-button-led

Per-peripheral example wrapping `<alp/peripheral.h>` GPIO via the
`<alp/chips/button_led.h>` block helper.  Polls a button and
toggles an LED.

## What this shows

- The portable button + LED block pattern: studio-resolved
  `pin_id` integers (`ALP_E1M_GPIO_IO0` / `ALP_E1M_GPIO_IO1`) feed
  `alp_button_led_init`.
- Active-low button convention.
- Press detection via polling (interrupt path is the helper's
  `alp_button_led_set_press_callback`).

## Build

```bash
west build -b native_sim/native/64 examples/gpio-button-led \
    -- -DEXTRA_ZEPHYR_MODULES=$(pwd)
west build -t run
```

## Reference

- [`<alp/chips/button_led.h>`](../../include/alp/chips/button_led.h)
- [`<alp/peripheral.h>`](../../include/alp/peripheral.h) GPIO surface
