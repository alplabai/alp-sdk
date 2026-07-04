# Portability cookbook

> Customer-facing walkthrough of the alp-sdk's load-bearing
> portability promise.  Read this if you are evaluating "if I write
> firmware for one SoM, can I really target another with no source
> changes?".  The short answer is **yes — within a SoM family**;
> the rest of this doc is the long answer with code, examples, and
> the failure modes the SDK will *not* protect you from.

## Companion documents

This cookbook ties three other docs together:

- [`docs/portability-matrix.md`](portability-matrix.md) — the empirical
  guarantee.  Every cell is a SKU × example compile test;
  21 / 21 cells green for E1M, 12 / 12 for E1M-X.
- [`docs/adr/0011-intra-family-portability.md`](adr/0011-intra-family-portability.md)
  — the architectural decision record that ratifies the intra-family
  boundary, with the alternatives we considered and rejected.
- [`docs/porting-new-som.md`](porting-new-som.md) — the porting-side
  flow: what a new SoM author does so a *future* `som.sku:` swap
  becomes a one-line edit.

If you are a customer adding firmware on top of the SDK, this doc
is your entry point.  If you are adding a *new* SoM beneath the
SDK, start with `porting-new-som.md` instead.

---

## 1. What "swap-and-run" actually means

The headline promise:

> Change `som.sku:` in `board.yaml`, rebuild, ship.

A concrete example — every cell in `docs/portability-matrix.md` is
a SKU swap of exactly this form, on real example sources, with no
other edits.

The promise has a **scope**.  It is not "any SoM, any time".  It is:

### Scope — INTRA-family

- **E1M family.**  `E1M-AEN301` ↔ `E1M-AEN401` ↔ `E1M-AEN501` ↔
  `E1M-AEN601` ↔ `E1M-AEN701` ↔ `E1M-AEN801` ↔ `E1M-NX9101`.
  Same 35 × 35 mm form factor, same `<alp/e1m_pinout.h>` symbol
  namespace, same E1M-spec instance reservations
  (`ALP_E1M_I2C_COUNT == 2`, `ALP_E1M_PWM_COUNT == 8`, etc.).
- **E1M-X family.**  `E1M-V2N101` ↔ `E1M-V2N102` ↔ `E1M-V2M101` ↔
  `E1M-V2M102`.  Same 45 × 65 mm form factor, same
  `<alp/e1m_x_pinout.h>` namespace, same E1M-X-spec reservations
  (`ALP_E1M_X_PCIE_COUNT == 1`, `ALP_E1M_X_ETH_COUNT == 2`, …).

Within each family the SDK guarantees that an app's source compiles
unchanged across every SKU; the generated `alp.conf` differs only in
silicon-determined deltas (Ethos-U variant, NPU driver enable, on-
module chip-driver set).  See
[`docs/portability-matrix.md`](portability-matrix.md) for the
field-by-field diff catalogue.

### Non-scope — cross-form-factor

Going **between** the families (E1M ↔ E1M-X) is **not** a portability
promise.  An app written for `E1M-AEN701` does not source-compile
on `E1M-V2N101` and vice versa, by design:

- different physical pinouts (the board-side spec differs);
- different power envelopes (mW-class M-only vs W-class A+M);
- different SoC architecture (Cortex-M-only vs heterogeneous
  Cortex-A55 + Cortex-M33);
- different NPU choices (Ethos-U / DRP-AI / DEEPX);
- and most visibly to your source code, **different header
  namespaces** — `<alp/e1m_pinout.h>` exports `ALP_E1M_PWM0` etc., while
  `<alp/e1m_x_pinout.h>` exports `ALP_E1M_X_PWM0` etc.  These are
  intentionally distinct symbols; mixing them is a build error.

The rationale for keeping the two product lines distinct is in
[`docs/adr/0011-intra-family-portability.md`](adr/0011-intra-family-portability.md);
the short form is "merging the two namespaces implies a false
equivalence we don't want customers to make at SoM-selection time".

### What that means in practice

If you choose a family at the start of your project, the SDK
guarantees you can swap SKUs *within* that family for the lifetime
of the firmware — including SKUs that ship after your initial
release, provided they belong to the same family.  AEN401 carries
an Ethos-U85 alongside its U55 pair; AEN701 only has U55s.  The
same source builds on both, the runtime picks the best available
NPU.  If next year's hypothetical AEN901 lands a different mix,
your app picks it up by editing one line of YAML and rebuilding.

---

## 2. The swap-test recipe

The day-to-day workflow is identical to writing firmware against
a single SoM, except that the field you edited to set your initial
target is the same field you edit to change targets.

### 2.1  Same-class swap: AEN701 → AEN801

This is the cleanest case — two SKUs in the same family, with
different silicon tiers (E7 → E8) but compatible board shapes
and identical app-facing pinout.  We'll walk through it using the
canonical `examples/peripheral-io/i2c-scanner/` example.

**Starting `examples/peripheral-io/i2c-scanner/board.yaml`:**

```yaml
som:
  sku: E1M-AEN701

preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals:
      - i2c

diagnostics:
  log_level: info
```

**Edit:** change one line.

```yaml
som:
  sku: E1M-AEN801          # was: E1M-AEN701

preset: e1m-evk
cores:
  m55_hp:
    app: ./src
    peripherals:
      - i2c

diagnostics:
  log_level: info
```

**Inspect the generated config (no build required yet):**

```bash
python scripts/alp_project.py \
    --input examples/peripheral-io/i2c-scanner/board.yaml \
    --core m55_hp \
    --emit zephyr-conf
```

You'll see the SoC selector switch from `E7` to `E8`, the on-
module chip enables stay the same (CC3501E, OPTIGA Trust M,
RV3028C7, TMP112, EEPROM 24C128 — every AEN carries the same on-
module BOM), and a new Ethos-U85 driver line appears alongside
the U55 line:

```
- CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
+ CONFIG_ALP_SOC_ALIF_ENSEMBLE_E8=y
  CONFIG_ALP_SDK_CHIP_CC3501E=y
  CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U55=y
+ CONFIG_ALP_SDK_INFERENCE_ETHOS_U_VARIANT_U85=y
  CONFIG_I2C=y
```

(`CONFIG_I2C=y` is the Zephyr-subsystem class symbol the loader
emits per `peripherals: [i2c]`; the Alp wrapper Kconfigs
`ALP_SDK_PERIPH_*` flip `default y` once the underlying class is
on.  See the `_PERIPHERAL_KCONFIG` table in
[`scripts/alp_project.py`](../scripts/alp_project.py).)

The `_U85=y` line is the visible footprint of fix G-1 (resolved
2026-05-18 — see the matrix doc).  Before that fix, AEN801 and
AEN701 generated byte-identical `alp.conf` for inference workloads;
now the orchestrator walks the SoM preset's
`inference.npu_population[]` and emits one variant-specific switch
per NPU present, so the TFLM driver can select Vela's
TensorOptimized kernels at link time on E8 silicon without losing
the U55 dispatch on the same SoM.

**Build:**

```bash
west build -b alp_e1m_aen801_m55_hp examples/peripheral-io/i2c-scanner
```

**Flash:** the `west flash` step is unchanged from any other AEN
target — the per-SoM bring-up doc (see
[`docs/bring-up-aen.md`](bring-up-aen.md)) explains the JLink
incantation, USB-DFU fallback, and the `west alp-flash` wrapper
for multi-image SoMs.

That is the swap test.  No source change.  The `main.c` from
`examples/peripheral-io/i2c-scanner/src/main.c` opens `ALP_E1M_I2C0` via
`alp_i2c_open()`, gets a real handle on both SKUs, scans the
bus.  Same code, same diagnostic output, the SDK just routed
through a different SoC under the hood.

### 2.2  Capability-gain swap: V2N101 → V2M101 (the DEEPX delta)

V2N101 carries the Renesas RZ/V2N silicon with its on-die DRP-AI3
NPU.  V2M101 is the same silicon plus a DEEPX DX-M1 NPU on a PCIe-
mux daughter footprint.  Same E1M-X form factor, same board
shape.  The swap demonstrates how the SDK exposes a *gained*
capability without source change.

**Edit `board.yaml`:**

```yaml
som:
  sku: E1M-V2M101          # was: E1M-V2N101

preset: e1m-x-evk
cores:
  a55_cluster:
    app: ./linux
    image: alp-image-edge
  m33_sm:
    app: ./m33_sm
    peripherals: [adc, pwm, i2c]
    libraries: [cmsis_dsp]
    inference:
      default_arena_kib: 256

diagnostics:
  log_level: info
```

**Inspect:**

```bash
python scripts/alp_project.py \
    --input board.yaml --core m33_sm --emit zephyr-conf
```

You'll see three new chip-driver lines appear:

```
+ CONFIG_ALP_SDK_CHIP_DEEPX_DXM1=y       (the NPU)
+ CONFIG_ALP_SDK_CHIP_PI3DBS12212=y      (PCIe mux)
+ CONFIG_ALP_SDK_CHIP_TPS628640=y        (DEEPX rail buck)
```

And, on the Linux/CMake side
(`--emit cmake-args`), one new switch:

```
+ -DALP_SDK_USE_DEEPX_DXM1=ON
```

(DEEPX lives on the Linux PCIe path, not Zephyr.)

The DRP-AI3 engine enable (`-DALP_SDK_USE_DRPAI_V2N=ON`, also on the
Linux/CMake side — the engine is A55-driven via the MERA runtime, so
there is no Zephyr Kconfig for it) is unchanged — both SKUs carry the
same RZ/V2N silicon, so DRP-AI3 is present on both.  V2M101 lights up
DEEPX *in addition*.

**The application code does not change.**  An app written like
this, on V2N101, picks DRP-AI3.  The same app, on V2M101, can
pick DEEPX explicitly *or* let the SDK pick:

```c
#include "alp/inference.h"

/* Compile-time identical on both V2N101 and V2M101; on the V2M
 * SKU the dispatcher picks DEEPX because it is the higher-tier
 * accelerator present; on the V2N SKU it picks DRP-AI3.  No
 * source change between the two. */
static alp_inference_t *load_my_model(const void *vela_bytes, size_t n)
{
    alp_inference_config_t cfg = {
        .model_data  = vela_bytes,
        .model_size  = n,
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .backend     = ALP_INFERENCE_BACKEND_AUTO,   /* dispatcher picks */
        .arena_bytes = 256u * 1024u,
        .arena       = NULL,                         /* heap-allocate */
    };
    return alp_inference_open(&cfg);
}
```

Two things make this work portably:

1. The SoM preset declares which NPUs are physically present
   (`inference.preferred_backend`, `inference.npu_population[]`,
   `capabilities.*` in `metadata/e1m_modules/<SKU>.yaml`).  Per
   memory note [[silicon-determined-fields-not-customer-facing]],
   that declaration is the *single source of truth* — the customer
   does not pick the backend, the silicon does.
2. The loader compiles in **every** dispatcher the active SoM
   declares (the V2M presets emit *both* DRP-AI3 and DEEPX
   enables, concurrently); the runtime then picks per
   `alp_inference_open()` call.  This is why the `inference:` block
   in `board.yaml` has no `backend:` field — see the schema
   description on `cores.<id>.inference` in
   [`metadata/schemas/board.schema.json`](../metadata/schemas/board.schema.json),
   which spells out exactly this contract.

If you specifically want DEEPX (say, for a benchmark run), pass
`.backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1` instead of AUTO.  If
that backend isn't present on the active SoM, `alp_inference_open()`
returns NULL and `alp_last_error()` returns `ALP_ERR_NOSUPPORT` —
see Section 5 for the runtime fallback pattern that uses this.

---

## 3. Dual namespace — `ALP_E1M_*` vs `ALP_E1M_X_*`

The headers under `include/alp/` deliberately expose two parallel
pinout namespaces, one per form factor.

### Why two

| Concern | E1M family | E1M-X family |
| --- | --- | --- |
| Mechanical | 35 × 35 mm | 45 × 65 mm |
| SoC class | Cortex-M only (Alif Ensemble, NXP i.MX 93 RT core) | Heterogeneous Cortex-A55 + Cortex-M33 (Renesas RZ/V2N) |
| Power envelope | mW-class | W-class |
| NPU options | Ethos-U55 / U65 / U85 | DRP-AI3 (V2N), DRP-AI3 + DEEPX DX-M1 (V2M) |
| Board | `E1M-EVK` or compatible | `E1M-X-EVK` or compatible |
| GPIO count | 26 (`ALP_E1M_GPIO_IO0..IO25`) | 36 (`ALP_E1M_X_GPIO_IO0..IO35`) |
| Ethernet | 1 MAC (`ALP_E1M_ETH0`) | 2 MAC (`ALP_E1M_X_ETH0`, `ALP_E1M_X_ETH1`) |
| PCIe | not routed | 1 instance (`ALP_E1M_X_PCIE0`) |
| Header | `<alp/e1m_pinout.h>` | `<alp/e1m_x_pinout.h>` |

A flat single-namespace alternative was considered and rejected
in ADR 0011 — it would imply false equivalence between two
product lines that solve very different workload shapes, and it
would force one form factor to lose useful pads (the E1M-X
GPIO_IO27..IO35 set, for example) to fit the narrower line's
range.

### When to use which

A decision tree:

- Battery-powered or coin-cell IoT sensor node, mW-class TDP?
  → **E1M family**, `<alp/e1m_pinout.h>`.
- Vision pipeline with multi-camera, Ethernet/PCIe storage, or
  Linux user-space?  → **E1M-X family**,
  `<alp/e1m_x_pinout.h>`.
- ML at the edge with Transformer/large-conv models that benefit
  from a 512-MAC accelerator?  → either family works (AEN E4/E6/
  E8 with Ethos-U85, or V2M with DEEPX DX-M1).  The cost / TDP /
  board-spec deltas drive the call.
- Headless audio DSP (wake-word, noise suppression)?  → either
  family; AEN's Helium MVE is a strong baseline at low power,
  V2N's NEON + DRP-AI3 covers heavier acoustic models.

The customer-side decision happens **once**, at SoM selection time
— before you write `#include`.  The SDK does not try to "lift"
that choice into runtime; you pick the form factor, your source
includes the matching pinout header, and from that point on every
SKU within the family is a one-line `som.sku:` swap.

### The "I want both" answer

There is no silver bullet — the two form factors are different
products.  The practical mitigation is that the **cross-cutting**
SDK surface is shared:

- `<alp/peripheral.h>` — same `alp_i2c_open()` / `alp_spi_open()` /
  `alp_uart_open()` shape on both lines.
- `<alp/inference.h>` — same dispatcher contract; AUTO works on
  both.
- `<alp/log.h>`, `<alp/rpc.h>`, `<alp/iot.h>` — uniform across
  both lines.

Only the pinout `#include` and the `som.sku:` line differ between
an E1M-targeting app and its E1M-X sibling.  Most projects that
want both maintain two thin app entry points that share one set
of business-logic translation units; the SDK's portable surface
keeps the diff minimal.

---

## 4. When NOT to expect portability

Honesty is more useful than reassurance.  These are the categories
of change that **do not** travel across a `som.sku:` swap, even
within a family — knowing them up front saves debugging time.

### 4.1  NPU model artefacts (the Vela / DRP-AI / DXNN files)

`<alp/inference.h>` abstracts the **dispatcher**, not the **model
bytes**.

A TFLite model compiled with Vela targeting Ethos-U55 will not
run on Ethos-U85 without a recompile against the matching
accelerator-config tag.  Likewise, a model compiled with the
Renesas DRP-AI translator toolchain does not run on Ethos-U, and
a DEEPX DXNN binary does not run on DRP-AI3 or Ethos-U.

That is silicon reality — each NPU has its own opcode set, memory
layout, and quantisation scheme.  The matching SDK contract is:

- **Portable.**  The `alp_inference_open()` call site, the tensor
  descriptor layout (`alp_inference_tensor_t`), the
  `alp_inference_invoke()` / `_close()` lifecycle.  Identical
  across every SoM in both families.
- **Not portable.**  The model file you pass in
  `cfg.model_data`.  You either ship one model file per NPU
  variant and select at boot time, or constrain your
  product to a single NPU variant per release.

The model-bytes responsibility is intentionally outside the SDK
per memory note [[silicon-determined-fields-not-customer-facing]]:
the SoM preset declares which NPU is present, the build compiles
the matching dispatcher in, the customer ships the matching model.

Two introspection helpers ship today to help validate the chain:

- `alp_inference_tflm_npu_variant_name()` — returns the
  highest-tier Ethos-U variant the build linked (e.g. `"u85"` on
  AEN801, `"u55"` on AEN701, `"u65"` on NX9101).  Use this to
  log a one-line "we are dispatching to NPU=u85" on boot and
  cross-check against the Vela target your model-compile pipeline
  produced.
- `alp_inference_tflm_cpu_kernel_variant()` — returns
  `"helium"`, `"neon"`, or `"ref"` per the CPU class of the
  active slice.  Useful when a model falls through to the CPU
  reference kernels and you want to confirm the SIMD path
  actually engaged.

Both are exported as `extern "C"` from
`src/zephyr/inference_tflm.cpp` for Zephyr builds; both return
static string-literal pointers, callers must not free.

### 4.2  Form-factor differences (the namespace error)

Trying to use `ALP_E1M_PWM0` on an E1M-X SoM is a build error — the
symbol doesn't exist in `<alp/e1m_x_pinout.h>`.  This is **by
design**.

```c
/* E1M-X target, but the include + symbol disagree: */
#include "alp/e1m_pinout.h"          /* WRONG header for V2N101 */
#include "alp/pwm.h"

alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
    .channel_id = ALP_E1M_PWM0,          /* WRONG namespace */
    .period_ns  = 1000000u,
});
```

The compiler will accept this (the macros expand to integers), but
the routes the SDK loads come from the E1M-X form-factor pinout —
your channel id may resolve to the wrong pad, or to no pad at all,
and `alp_pwm_open()` returns NULL.  The fix is one-line:

```c
#include "alp/e1m_x_pinout.h"        /* correct header */
#include "alp/pwm.h"

alp_pwm_t *led = alp_pwm_open(&(alp_pwm_config_t){
    .channel_id = ALP_E1M_X_PWM0,        /* correct namespace */
    .period_ns  = 1000000u,
});
```

If you must keep one source tree that switches per form factor,
the build identifier `ALP_HW_BUILD_SOM_FAMILY` (a string macro
emitted into the auto-generated `alp_hw_info_build.h` by the
loader — see the emitter in `scripts/alp_project.py`) is the
canonical source of truth.  Customers more often pass a project-
defined `MY_PRODUCT_USES_E1M_X` macro via CMake (`target_compile_definitions`)
and key the `#include` off it:

```c
#if defined(MY_PRODUCT_USES_E1M_X)
#  include "alp/e1m_x_pinout.h"
#  define MY_LED  ALP_E1M_X_PWM0
#else
#  include "alp/e1m_pinout.h"
#  define MY_LED  ALP_E1M_PWM0
#endif
```

…but two-line `#ifdef` walls quickly become a code smell.  The
prevailing pattern is to keep one entry point per form factor and
share the business-logic translation units below.  See Section 3.

### 4.3  Heterogeneous-OS topology choices

An app written for the M55-HP slice of an AEN does **not** auto-
port to the A55 cluster of a V2N.  Different OS (Zephyr vs Yocto
Linux), different threading model, different supervisor structure,
and different inference dispatch (Ethos-U on Cortex-M, DRP-AI on
Cortex-A).  Even within a single SoM, the per-cluster apps live
in different `cores.<key>:` entries in `board.yaml` and consume
different `<alp/...>` surfaces.

[`docs/heterogeneous-builds.md`](heterogeneous-builds.md) walks
through the per-core fan-out for the heterogeneous V2N101 case.
The relevant portability claim is "within a `cores.<key>:` slice,
the source is portable across SKUs in the family that share that
core class" — *not* "the source is portable across cores within
one SoM".

Concretely: a V2N101 → V2M101 swap is portable on the `m33_sm`
slice (both SKUs run Zephyr on the system manager M33) and on the
`a55_cluster` slice (both SKUs run Yocto Linux on the A55s).  The
A55-side app and the M33-side app are still distinct source trees;
the portability is *within* each slice, not across slices.

### 4.4  Board-specific hardware

If your code calls `alp_camera_open()` with the OV5640 driver, it
will not work on a board where OV5640 is not populated — the
chip is on the board, not on the SoM.  The SDK reads
`board.populated:` in `board.yaml` (or the board preset's
default population list) to decide which `CONFIG_ALP_SDK_CHIP_*=y`
lines to emit; flipping the board flips that set.

The relevant rule: **SoM swap ≠ board swap.**  The portability
matrix specifically tests SoM swaps with the board held
constant.  If you also need to migrate boards, that is a
separate audit — typically a one-time per-product decision.

Two helpful patterns:

- For optional board components, gate the open call on the
  matching chip enable, e.g.
  `#ifdef CONFIG_ALP_SDK_CHIP_OV5640` around the
  `alp_camera_open()` block and fall back to a "no camera"
  diagnostic mode.
- For mandatory components, declare them in `chips:` at the top
  of `board.yaml` so the build fails fast if the board doesn't
  populate them.

---

## 5. Capability validation — runtime, not `#ifdef`

The SDK is built to favour **runtime detection** of capability gaps
over compile-time guards.  This keeps the same source compiling
across every SKU in a family, and pushes the decision about how
to react to a missing capability up into the application — where
the right decision is product-specific anyway.

### 5.1  The contract

Every `alp_*_open()` function in the SDK shares one contract:

- On success, returns a non-NULL handle.
- On failure, returns **NULL** and writes a thread-local error code
  readable via `alp_last_error()`.

The error codes that matter for portability work are:

| Code                  | Meaning                                                  |
| --------------------- | -------------------------------------------------------- |
| `ALP_OK`              | No error since the last successful open() on this thread |
| `ALP_ERR_INVAL`       | NULL config, bus id out of range                         |
| `ALP_ERR_NOT_READY`   | DT alias unset / device not ready (typical on emul)      |
| `ALP_ERR_NOSUPPORT`   | Backend lacks this feature on the active SoM             |
| `ALP_ERR_OUT_OF_RANGE`| Config exceeds the active SoC's documented caps          |

`ALP_ERR_NOSUPPORT` is the load-bearing one for portability work.
It means "the SDK compiled fine, the SoM is real, but this
specific capability isn't available on the active silicon".  The
canonical example is requesting `ALP_INFERENCE_BACKEND_DEEPX_DXM1`
on a V2N101 (no DEEPX) — the call returns NULL with
`alp_last_error() == ALP_ERR_NOSUPPORT`.

See the full Doxygen on `alp_last_error()` in
[`include/alp/peripheral.h`](../include/alp/peripheral.h) for the
thread-local semantics — concurrent `open()` calls on different
threads do not clobber each other's diagnostic, and a successful
open on a thread clears the slot.

### 5.2  Compile-time bounds — `<alp/soc_caps.h>` + `<alp/e1m_pinout.h>`

When you do want a compile-time bound, the SDK exposes two
generated headers:

- [`include/alp/soc_caps.h`](../include/alp/soc_caps.h) —
  auto-generated from the active SoC's JSON spec by
  `scripts/gen_soc_caps.py`.  Provides `ALP_SOC_<CLASS>_COUNT`
  upper-bound macros (`ALP_SOC_I2C_COUNT`, `ALP_SOC_PWM_COUNT`,
  `ALP_SOC_CAN_COUNT`, …) and boolean capability macros
  (`ALP_SOC_HELIUM_MVE`, `ALP_SOC_NEON`, `ALP_SOC_DRP_AI`,
  `ALP_SOC_INLINE_AES`, `ALP_SOC_GPU2D`, …).  Gated by
  `CONFIG_ALP_SOC_<TOKEN>`; when no SoC is selected the macros
  default to `UINT16_MAX` so capability checks accept any config
  (useful for portable libraries that don't lock to one SoC).
- [`include/alp/e1m_pinout.h`](../include/alp/e1m_pinout.h) /
  [`include/alp/e1m_x_pinout.h`](../include/alp/e1m_x_pinout.h) —
  the form-factor portability bound.  `ALP_E1M_I2C_COUNT == 2`
  means "every E1M-conformant SoM routes at least 2 I2C
  instances; you can use `ALP_E1M_I2C0..ALP_E1M_I2C1` portably; higher
  indices are vendor-specific extensions and may or may not be
  routed on the active SoM".

A `_Static_assert` against these is the right tool when your
firmware genuinely cannot work on smaller silicon:

```c
#include "alp/soc_caps.h"
#include "alp/peripheral.h"

/* Compile-time refusal on SoCs that physically can't run this
 * design (8 SPI buses isn't a portability hazard, it's a hard
 * silicon requirement). */
_Static_assert(ALP_SOC_SPI_COUNT >= 8,
               "this app requires >=8 SPI instances; "
               "select a richer SoC in board.yaml");
```

The pattern is "lean on runtime detection by default; reach for
`_Static_assert` only when the requirement is sharp enough that
silently degrading at runtime would be a worse customer experience
than a build error."

**Gate on capabilities, not board names.**  The umbrella header
[`include/alp/cap.h`](../include/alp/cap.h) wraps the raw counts in
`ALP_HAS(<CAP>)` (a compile-time constant expression, safe in `#if`
and `static_assert`) and `alp_has(ALP_CAP_ID_<CAP>)` (the runtime
twin).  Reaching for `#ifdef CONFIG_BOARD_<NAME>` to skip a
peripheral is the anti-pattern — it forks the source per board and
silently breaks when the next SoM arrives.  Ask the silicon instead:

```c
#include "alp/cap.h"

int main(void)
{
    /* Runtime gate: skip cleanly on ADC-less silicon.  Same source
     * runs on every SoM -- no #ifdef CONFIG_BOARD_* forks. */
    if (!alp_has(ALP_CAP_ID_HW_ADC)) {
        printf("no ADC on this SoC (%s) -- skipping\n", ALP_SOC_REF_STR);
        return 0;
    }

    /* Compile-time gate: the unused branch disappears from the
     * binary entirely on parts that lack the feature. */
#if ALP_HAS(HW_CAN_FD)
    /* ... configure ALP_CAN_MODE_FD + bitrate_data_hz ... */
#endif
    /* ... */
}
```

See
[`examples/peripheral-io/adc-voltmeter`](../examples/peripheral-io/adc-voltmeter/src/main.c)
for the pattern in a full example (capability gate → open →
instance-level `alp_adc_capabilities()`), and
[`examples/peripheral-io/hello-world`](../examples/peripheral-io/hello-world/src/main.c)
for the minimal teaching block.  Note that when no SoC is selected
(`native_sim`) the capability layer is permissive — gates pass and
the `alp_*_open()` contract from §5.1 provides the graceful failure.

### 5.3  Runtime detection — the fallback ladder pattern

For inference dispatch in particular, the dispatcher's `AUTO`
mode does the right thing most of the time:

```c
#include "alp/inference.h"

/* AUTO: dispatcher picks the highest-tier NPU present, falls
 * through to the CPU TFLM kernels if nothing maps.  Identical
 * call site on every supported SoM. */
alp_inference_t *m = alp_inference_open(&(alp_inference_config_t){
    .model_data  = vela_bytes,
    .model_size  = vela_size,
    .format      = ALP_INFERENCE_MODEL_TFLITE,
    .backend     = ALP_INFERENCE_BACKEND_AUTO,
    .arena_bytes = 256u * 1024u,
});
if (m == NULL) {
    /* Last-resort: nothing the SDK could load this model with.
     * Almost always means the model bytes don't match any of
     * the compiled-in dispatchers.  Inspect alp_last_error(). */
    LOG_ERR("inference: open failed: %d", (int)alp_last_error());
    return -1;
}
```

When a benchmark or A/B comparison needs an *explicit* backend,
the same pattern lets you walk a preference list, falling through
on `ALP_ERR_NOSUPPORT`:

```c
#include "alp/inference.h"

/* Try DEEPX -> DRP-AI3 -> Ethos-U -> CPU TFLM.  Each step is
 * portable: on a SoM that lacks the requested backend,
 * alp_inference_open() returns NULL with last_error =
 * ALP_ERR_NOSUPPORT and we drop to the next tier. */
static const alp_inference_backend_t ladder[] = {
    ALP_INFERENCE_BACKEND_DEEPX_DXM1,
    ALP_INFERENCE_BACKEND_DRPAI,
    ALP_INFERENCE_BACKEND_ETHOS_U,
    ALP_INFERENCE_BACKEND_CPU,
};

static alp_inference_t *open_preferred(const void *model, size_t n)
{
    alp_inference_config_t cfg = {
        .model_data  = model,
        .model_size  = n,
        .format      = ALP_INFERENCE_MODEL_TFLITE,
        .arena_bytes = 256u * 1024u,
    };
    for (size_t i = 0; i < sizeof(ladder) / sizeof(ladder[0]); i++) {
        cfg.backend = ladder[i];
        alp_inference_t *m = alp_inference_open(&cfg);
        if (m != NULL) {
            LOG_INF("inference: opened backend %u on attempt %zu",
                    (unsigned)cfg.backend, i + 1);
            return m;
        }
        if (alp_last_error() == ALP_ERR_NOSUPPORT) {
            /* Backend not present on this SoM; try the next tier. */
            continue;
        }
        /* Any other error -- model bad, OOM, etc. -- is fatal. */
        LOG_ERR("inference: open failed: %d", (int)alp_last_error());
        break;
    }
    return NULL;
}
```

This compiles unchanged on every SoM the SDK supports.  Each call
of `alp_inference_open()` with an explicit backend either succeeds
(backend is present, model loads) or fails fast with
`ALP_ERR_NOSUPPORT`.

The same pattern generalises to other capability classes — request
`alp_spi_open()` with `bus_id = ALP_E1M_SPI1` on a SoM that doesn't
route SPI1 and you get NULL + `ALP_ERR_NOSUPPORT`; fall through
to SPI0 or report the gap to the user as you see fit.

### 5.4  Why no `alp_inference_capabilities()` enumeration

Today the SDK does **not** ship a `alp_inference_capabilities()`
that returns "the list of backends this build linked".  The
explicit-backend + `ALP_ERR_NOSUPPORT` ladder above is the
recommended pattern for now; the introspection helpers
`alp_inference_tflm_npu_variant_name()` and
`alp_inference_tflm_cpu_kernel_variant()` (Section 4.1) cover
the "log on boot which NPU + CPU kernel got linked" case.

A capability-enumeration API may land in a future SDK release;
until then the ladder pattern is the load-bearing answer.

---

## 6. Per-family portability matrix (link)

The empirical guarantee — every cell a compile test, the diff
catalogue, and the open gaps — lives in
[`docs/portability-matrix.md`](portability-matrix.md).  Skim it
when you're picking SKUs or when you suspect the SDK isn't keeping
its promise.

Headline numbers as of 2026-05-18:

- **E1M family.**  21 / 21 (SKU × example) cells generate cleanly.
  All 6 AEN SKUs produce byte-identical `alp.conf` for every
  example, after stripping the SoC identity comment.  NX9101
  participates with its own Ethos-U65 dispatcher.
- **E1M-X family.**  12 / 12 cells generate cleanly.  V2M SKUs
  add three on-module chip-driver enables (DEEPX DX-M1, PCIe
  mux, DEEPX rail buck) but otherwise produce the same
  generated config as V2N within each example.

Two open gaps in the matrix today (both metadata-level, both
flagged):

- **A2-1** — `E1M-V2M102.yaml` declares its pad routes in the
  `E1M_*` namespace instead of `E1M_X_*`.  Apps using
  `<alp/e1m_x_pinout.h>` symbols resolve on V2N101 / V2N102 /
  V2M101 but miss on V2M102.  Single-file fix queued.
- **A2-2** — V2M101 + V2M102 are missing pad routes for
  `E1M_X_GPIO_IO27..IO35` (V2N101 / V2N102 carry them).  Awaits
  maintainer input per [[pending-hw-configs]] — we don't invent
  pin assignments.

Two recently-resolved gaps (the per-variant NPU and CPU-class
selectors):

- **G-1** — Ethos-U variant (U55 / U65 / U85) is now visible to
  the build; resolved 2026-05-18.  AEN401 / AEN601 / AEN801
  now emit both `_U55=y` and `_U85=y`; AEN301 / AEN501 / AEN701
  emit only `_U55=y`; NX9101 emits `_U65=y`.
- **G-2** — TFLM CPU-class kernel selector (NEON / Helium /
  REF) is now per-slice; resolved 2026-05-18.  M55_HP slices
  emit `_HELIUM=y`, A55 slices emit `_NEON=y`, M33 slices
  emit `_REF=y`.

### Future work — CI enforcement

The matrix is currently maintained **by hand** against the
evidence under `build/portability-test/` (gitignored).  Each
release cycle re-runs `scripts/alp_project.py` for every (SKU ×
example) cell and diffs the generated `alp.conf` to confirm the
matrix is still green.

Phase E.3 (future work, no release commitment) will land
`scripts/gen_portability_matrix.py` and a CI job that produces
the matrix mechanically per commit.  Until then, the matrix and
this cookbook are the customer-facing guarantee; the
implementation is verified each release against the matrix.

---

## 7. Where to next

- [`docs/portability-matrix.md`](portability-matrix.md) — the
  empirical guarantee.
- [`docs/adr/0011-intra-family-portability.md`](adr/0011-intra-family-portability.md)
  — the architectural rationale + alternatives considered.
- [`docs/porting-new-som.md`](porting-new-som.md) — what a *new*
  SoM author does to make their SKU a one-line `som.sku:` swap
  away.
- [`docs/architecture.md`](architecture.md) — full SDK map (OS
  targets, repository layout, the SDK ↔ studio boundary).
- [`docs/e1m-pinout.md`](e1m-pinout.md) — how E1M pads link the
  open-standard spec, the per-SoM `pad_routes:` block, and the
  board's `e1m_routes:` block.
- [`docs/heterogeneous-builds.md`](heterogeneous-builds.md) — the
  per-core fan-out walkthrough for heterogeneous SoMs.
- [`docs/firmware-quickstart.md`](firmware-quickstart.md) — the
  "I have hardware on the bench" entry point; complements this
  cookbook with board wiring + bring-up flow.
- Per-SoM bring-up notes —
  [`docs/bring-up-aen.md`](bring-up-aen.md),
  [`docs/bring-up-v2n.md`](bring-up-v2n.md),
  [`docs/bring-up-v2n-m1.md`](bring-up-v2n-m1.md),
  [`docs/bring-up-imx93.md`](bring-up-imx93.md) — covering
  flashing, debugger setup, and gotchas per silicon family.

If anything in this cookbook doesn't match what
`scripts/alp_project.py` or the generated `alp.conf` actually do,
the script + the matrix in [`docs/portability-matrix.md`](portability-matrix.md)
are canonical and this doc is wrong — please open an issue (or
send a patch).
