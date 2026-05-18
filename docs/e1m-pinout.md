# E1M pinout — and how the SDK consumes it

The ALP SDK **does not duplicate** the E1M pinout.  The standard
lives in [`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec)
and the SDK only sees opaque integers (`bus_id`, `pin_id`,
`port_id`) that have already been resolved through a chain of
inputs (the spec + this repo's per-SoM metadata).  This file
explains the chain and how each layer sees the pinout, so
contributors don't try to import pad data into this repo's source
tree.

**The per-SoM pad → silicon-pin layer also lives in this repo** (the
SoM preset's `pad_routes:` block under
[`metadata/e1m_modules/<SKU>.yaml`](../metadata/e1m_modules/)); only
the spec itself stays external.  (This is a 2026-05-18 reversal of an
earlier design that placed the routes in `alp-studio`; see the box
below.)

## The chain

```
+----------------------------------------------------------------+
|  alplabai/e1m-spec — the open standard                         |
|    pinout/v1.json     E1M (35×35), pads A1..AH34, default      |
|    pinout/x-v1.json   E1M-X (45×65), pads A1..AH50             |
|    STANDARD.md        normative prose, signal classes           |
+----------------------------------------------------------------+
                              │
                              │ pad → silicon pin mapping
                              ▼
+----------------------------------------------------------------+
|  Per-SoM pad routes (lives in alp-sdk, slice 2)                |
|    metadata/e1m_modules/<SKU>.yaml  -> pad_routes:             |
|    pad_routes:                                                 |
|      - pad: AF2                                                |
|        soc_pin: P3_4                                           |
|        as: I2C_SCL                                             |
|        instance: I2C0                                          |
+----------------------------------------------------------------+
                              │
                              │ pad routing + block requirements
                              ▼
+----------------------------------------------------------------+
|  alp-studio pin allocator                                      |
|    Picks an I2C/SPI/GPIO/UART instance per block, generates     |
|    init code that calls alp_*_open({.bus_id=N, ...}).          |
+----------------------------------------------------------------+
                              │
                              │ opaque integer N
                              ▼
+----------------------------------------------------------------+
|  ALP SDK — THIS REPO                                           |
|    alp_i2c_open(&cfg) resolves cfg.bus_id to a Zephyr          |
|    device pointer (zephyr backend) or a vendor HAL handle      |
|    (baremetal / yocto backends).                                |
+----------------------------------------------------------------+
                              │
                              ▼
                     Vendor HAL → CMSIS → Silicon
```

## What the SDK sees

The public surface (`<alp/peripheral.h>`) takes only **opaque
integers** for instance selection.  Those integers are
**fixed by the E1M standard** — every E1M-conformant carrier
resolves them identically — and the SDK ships them as named
macros in [`<alp/e1m_pinout.h>`](../include/alp/e1m_pinout.h):

```c
#include <alp/peripheral.h>
#include <alp/e1m_pinout.h>

alp_i2c_open(&(alp_i2c_config_t){
    .bus_id = E1M_I2C0,         /* always 0 across every E1M carrier */
    .bitrate_hz = 400000
});

alp_gpio_open(E1M_GPIO_IO7);    /* always pad AG34 — silicon-agnostic */
```

The SDK never references E1M pad IDs (`A1`, `AF2`, `R34`, …),
silicon pin names (`P3_4`, `VDD_5V_IN`, …), or SoM SKUs
(`E1M-AEN301`, …) in its public API.  It can't — those couplings
would break the cross-variant abstraction.

### Carrier-specific feature names

Names like `USER_LED_RED` or `ENCODER_SW` are **not** in the E1M
standard — they're carrier-specific.  Those live in
[`<alp/boards/<board>.h>`](../include/alp/boards/) as thin
re-exports of the underlying `E1M_GPIO_*` indices:

```c
#include <alp/boards/alp_e1m_evk.h>

alp_gpio_t *led = alp_gpio_open(EVK_PIN_LED_RED);
/* expands to alp_gpio_open(E1M_GPIO_IO0) — the standard
 * GPIO index, the readable name, both true at once. */
```

## What the studio sees

The studio is the **AI-driven visual programmer** that sits on top
of alp-sdk.  It reads alp-sdk metadata to power its pin allocator +
codegen for vibe-coded hardware projects.  Hand-written firmware
authors bypass the studio entirely and call `<alp/...>` directly.

The studio is the **only** layer that sees the full chain.  When it
imports a block with `interfaces.provides = ["alp_i2c"]`, it:

1. Reads the active SKU's SoM preset at
   [`metadata/e1m_modules/<SKU>.yaml`](../metadata/e1m_modules/) in
   alp-sdk (`pad_routes:` for pad → silicon routes, `silicon:` +
   `silicon_variant:` for the SoC + exact MPN).  alp-sdk's metadata
   is alp-studio's input, not its output.
2. Looks up which E1M pads expose `I2C_SCL` / `I2C_SDA` for that
   SKU and which `instance` value those routes name (`I2C0`,
   `I2C1`, …).
3. Picks a free instance for the block.
4. Emits codegen that translates the instance into the integer
   `bus_id` the SDK consumes.

(Until 2026-05-18 the SoM preset lived in alp-studio at
`library/_soms/<id>/manifest.json`; it now lives in alp-sdk so all
generator inputs sit in one repo.)

## What e1m-spec sees

The standard fixes:

- The set of physical pads (312 on E1M, 496 on E1M-X).
- The **default function** of each pad (e.g. `AF2` defaults to
  `I2C_SCL` for instance `I2C0`).
- The **GPIO secondary** available on every digital pad.
- Mechanical and electrical envelope.

It does **not** specify:

- Which silicon (chip) implements a given pad on a SoM.
- Which peripheral instances a SoM family routes (an SoM can route
  a strict subset; e.g. E1M-AEN routes only `ETH0_*`, not `ETH1_*`).
- Software / firmware of any kind.

## Version pinning

The SDK's v0.1 release is built against **e1m-spec v1.1** (the
first public release).  The pin is referenced explicitly in
`west.yml` so a `west update` on this SDK pulls a known-compatible
spec revision.

If you're adding support for a new SoM or carrier board:

1. The pad-level routing of your SoM goes into a new entry under
   [`metadata/e1m_modules/<SKU>.yaml`](../metadata/e1m_modules/)
   in this repo (the `pad_routes:` block, introduced as of slice 2
   of the metadata unification work).  See
   [`e1m-spec/examples/alp-aen.som-manifest.json`](https://github.com/alplabai/e1m-spec/blob/main/examples/alp-aen.som-manifest.json)
   for the shape (the spec's JSON example translates 1:1 into the
   YAML block).
2. The chip-level metadata (cores, peripherals, NPUs, packages,
   variants) goes into [`metadata/socs/<vendor>/<family>/<part>.json`](../metadata/socs/)
   in this repo.
3. The vendor wrapper (HAL bindings) goes into
   [`vendors/<som-slug>/`](../vendors/) here.
4. The Zephyr board file goes into
   [`alplabai/alp-zephyr-modules`](https://github.com/alplabai/alp-zephyr-modules) — not here.

The SDK code never reads pad IDs.  If you find yourself wanting to
write `if (pad == "AF2")` somewhere in `src/`, stop — that
dependency belongs higher in the chain, in the studio's pin
allocator.

## See also

- [`docs/architecture.md`](architecture.md) — the SDK's layering.
- [`docs/porting-new-som.md`](porting-new-som.md) — checklist for
  adding a new SoM, including the pinout / manifest split.
- [`vendors/alif/README.md`](../vendors/alif/README.md) — concrete
  example of E1M-AEN routing caveats (only `ETH0_*` routed, no
  PCIe, etc.).
- [`alplabai/e1m-spec`](https://github.com/alplabai/e1m-spec) —
  the canonical pinout standard.
