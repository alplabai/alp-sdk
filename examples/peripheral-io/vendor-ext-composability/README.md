# vendor-ext-composability

One pad, three ways -- a proof that the Alp SDK's vendor-extension
layer (`<alp/ext/<vendor>/...>`) is **additive, never exclusive**.

Claiming a vendor-specific feature on a pad does *not* lock that pad
out of the portable surfaces. The same physical pad stays usable as a
plain digital GPIO and as its portable E1M peripheral. This example
exercises all three surfaces on the Arduino A1 analog input
(`EVK_ADC_ARDUINO_A1` = `E1M_ADC1`):

| Way | Surface | Call |
|-----|---------|------|
| 1 | plain GPIO | `alp_gpio_open(E1M_GPIO_ADC1)` -- the pad's universal GPIO secondary (index 43 in the positional `e1m_pinout.h` order) |
| 2 | portable ADC | `alp_adc_open(EVK_ADC_ARDUINO_A1)` -- the cross-vendor `<alp/adc.h>` surface, identical on every E1M SoM |
| 3 | vendor extension | `alp_alif_adc_set_trigger_source(handle)` -- an Alif-only knob from `<alp/ext/alif/adc.h>`, layered **on the Way-2 handle** |

The Way-3 extension *augments* the portable handle; it never replaces
it. On non-Alif silicon the extension call returns
`ALP_ERR_NOT_PRESENT_ON_THIS_SOC` and the portable Way-2 read keeps
working unchanged. The header is pulled in defensively with
`__has_include`, so this example compiles on every SoM.

## Why this matters

A pad is a shared resource. The SDK guarantees that reaching for a
vendor knob never strands the pad: an app can still fall back to the
portable peripheral or to a bare GPIO. That guarantee is what lets you
prototype against the portable surface, opt into a vendor accelerator
where it pays off, and stay portable everywhere else -- on the same
pins, in the same firmware.

## Running under native_sim

```bash
west alp-build -b native_sim/native/64 examples/peripheral-io/vendor-ext-composability
west build -d build -t run
```

The host overlay wires a GPIO-emul controller, so **Way 1 actually
toggles the pad**. It wires no ADC controller, so **Ways 2/3 take the
graceful `skip` path** (`alp_adc_open` returns `NULL` with
`ALP_ERR_NOT_READY`) -- the same NULL-tolerant contract every SDK
`open()` honours. On a real Alif EVK all three ways run for real.

Reaching `E1M_GPIO_ADC1` (index 43) needs two GPIO-emul controllers,
since a single emul controller caps at 32 pins; the overlay shows the
canonical two-controller split a full 52-slot board file uses.

## Build (on the EVK)

Pending the `alp_e1m_evk_aen` board file in
[`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules):

```bash
west build -b alp_e1m_evk_aen examples/peripheral-io/vendor-ext-composability
west flash
```
