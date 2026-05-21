# Backend Registry + Capability Negotiation Architecture

**Date:** 2026-05-21
**Status:** Draft — pending implementation
**Owner:** alpCaner
**Predecessor work:** PR #14 (`ALP_HAS` capability macros), PR #15 (rich diagnostics), PR #16 (`alp` CLI subcommands)

## Motivation

The 2026-05-21 SoC-unification audit (see chat record) revealed that today's portable-API layer dispatches via `#if` ladders inside one `.c` file per OS layer per subsystem. Concrete consequences:

- **N×M maintenance.** Adding a new SoM family means touching every backend file. Adding a new peripheral category means writing one new file per SoM family.
- **Stubs lie silently.** `storage_stub.c` returns `ALP_ERR_NOSUPPORT` on every call; customers cannot tell whether the hardware is absent, the SDK is incomplete, or their config is wrong.
- **Vendor-name leaks through documentation.** Public-header comments name "V2N today, AEN E3/E5/E7 without ISP" — surface-level information that should be queryable, not narrated.
- **Combinatorial Kconfig.** Inference is gated by `CONFIG_ALP_SDK_INFERENCE_{ETHOS_U,DRPAI,DEEPX_DX,TFLM}` — every new backend doubles the matrix.
- **No vendor-specific extension surface.** Customers who need Alif hardware oversampling on ADC have to drop to `chips/<part>/` driver code, which the SDK convention explicitly forbids for app code.

This spec replaces the `#if` ladder with a linker-section registry, adds runtime capability negotiation, gives vendor-specific knobs a first-class public surface, formalises the software fallback tier, and replaces the silent stub pattern with three distinct error codes.

## Non-goals

- Backend ABI versioning (`alp_backend_t` may evolve through v0.7–0.8; ABI snapshot covers it).
- Cross-language bindings (Rust, Python).
- Runtime backend hotplug (USB camera, removable storage).
- Display + GPU2D migration is deferred past v1.0 (no backend exists today; design recorded, implementation later).
- Legacy customer compatibility — no customers exist; hard cutover per subsystem is acceptable.

---

## Section 1 — Registry mechanism

### Public header

A new header `<alp/backend.h>` carries the registration macro and the runtime walker:

```c
typedef struct alp_backend_ctx { /* class-specific extension storage */ } alp_backend_ctx_t;

typedef struct alp_backend {
    const char       *silicon_ref;    /* "alif:ensemble:e7", "renesas:rzv2n:n44", "*" for SW */
    const char       *vendor;         /* "alif", "renesas", "nxp", "gd32", "sw" */
    uint32_t          base_caps;      /* static ALP_INSTANCE_CAP_* bitfield */
    uint8_t           priority;       /* tie-break; SW=0, HW=100, specialised HW=110 */
    const void       *ops;            /* class-specific ops vtable */
    int             (*probe)(uint32_t instance_id, uint32_t *refined_caps);
                                      /* optional; NULL = use base_caps as-is */
} alp_backend_t;

#define ALP_BACKEND_REGISTER(class, name, ...)                           \
    static const alp_backend_t _alp_be_##class##_##name                  \
        __attribute__((used, section(".alp_backends_" #class))) =        \
        __VA_ARGS__

const alp_backend_t *alp_backend_select(const char *class_name,
                                         const char *silicon_ref);
```

### Selection algorithm

`alp_backend_select` walks the section between `__start_.alp_backends_<class>` and `__stop_…`, filters by `silicon_ref` exact match (or wildcard `"*"` for SW fallback), sorts by `priority` desc, returns the first hit. Cached after first call per class.

### Toolchain support

- GCC / Clang: section attribute + `__start_` / `__stop_` symbols are first-class.
- IAR / ARMCC: pragma equivalents wrapped by `ALP_BACKEND_REGISTER` portability layer.
- MSVC (host tools only): `#pragma section` + `__pragma(comment(linker, "/MERGE:…"))`.

### CI gate against linker stripping

A new regression test `tests/regress/test_backend_section_visibility.c` registers a sentinel backend at link time and asserts the walker finds it. Fails fast if a linker script discards the section.

### Why a section, not constructors

Zero runtime init cost. No constructor ordering problems. Tree-shakable — `#ifdef`-out a backend file and its entry vanishes from the section automatically. Debuggable via standard linker map. Same pattern Zephyr uses for `STRUCT_SECTION_ITERABLE` / `DEVICE_DEFINE`.

---

## Section 2 — Capability struct + runtime negotiation

### Two distinct namespaces

| Namespace | Level | Compile-time | Already shipped? |
|---|---|---|---|
| `ALP_CAP_*` macros, `ALP_HAS()` | SoC-level (does silicon have an NPU at all?) | Yes — constant expression | Yes — PR #14 |
| `ALP_INSTANCE_CAP_*` flags | Handle-level (does THIS opened ADC support DMA?) | No — runtime, queryable | New, this spec |

### Types

```c
typedef enum {
    ALP_INSTANCE_CAP_DMA            = 1u << 0,
    ALP_INSTANCE_CAP_HW_OVERSAMPLE  = 1u << 1,
    ALP_INSTANCE_CAP_HW_TRIGGER     = 1u << 2,
    ALP_INSTANCE_CAP_DIFFERENTIAL   = 1u << 3,
    /* ...class-specific entries added as subsystems migrate... */
} alp_instance_cap_t;

typedef struct alp_capabilities {
    uint32_t flags;            /* OR of alp_instance_cap_t */
    uint32_t max_sample_rate;  /* 0 = N/A */
    uint16_t max_resolution_bits;
    uint16_t channel_count;
    /* ...class-specific fields populated by ops->probe... */
} alp_capabilities_t;

bool alp_capabilities_has(const alp_capabilities_t *c, alp_instance_cap_t f);

/* Every migrated handle gains a getter. */
const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *h);
const alp_capabilities_t *alp_spi_capabilities(const alp_spi_t *h);
/* ...etc... */
```

### Customer flow

```c
alp_adc_t *h = alp_adc_open(&cfg);
const alp_capabilities_t *caps = alp_adc_capabilities(h);
if (alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_OVERSAMPLE)) {
    /* fast path */
} else {
    /* portable fallback */
}
```

### How caps get filled in

1. Backend declares `base_caps` at registration (common case).
2. SDK calls `ops->probe(instance_id, &refined_caps)` on open if probe is non-NULL; backend can refine downward per-instance.
3. Result cached in the handle; `alp_X_capabilities()` is a pointer return.

---

## Section 3 — Vendor extension surface

### Header layout

```
include/alp/ext/
├── alif/
│   ├── adc.h          alp_alif_adc_set_oversampling, _set_trigger_source, ...
│   ├── camera.h       alp_alif_camera_isp_set_3a_window, ...
│   └── inference.h    alp_alif_inference_ethos_u_vela_options, ...
├── renesas/
│   ├── adc.h
│   ├── camera.h       alp_renesas_camera_isp_*    (V2N RZ/V2N N44 ISP fabric)
│   ├── inference.h    alp_renesas_inference_drpai_*
│   └── tmu.h          alp_renesas_tmu_cordic_*
├── nxp/
│   ├── adc.h
│   └── inference.h    alp_nxp_inference_ethos_u65_*
└── deepx/
    └── inference.h    alp_deepx_inference_dxm1_*
```

### Naming convention

`alp_<vendor>_<class>_<verb>(...)`. First arg is the portable handle (`alp_adc_t *`, etc.). Same prefix as the portable surface — only the `<vendor>` infix signals non-portability.

### Discoverability

- Compile-time: each header defines `ALP_EXT_<VENDOR>_<CLASS>_AVAILABLE 1`.
- Documentation: Doxygen `@par Supported silicon:` tag on every public extension function. CI gate validates the tag exists.
- Catalogue: `docs/extensions/index.md` auto-generated from the headers.

### Runtime safety

Every extension function verifies the handle's backend matches its expected vendor; returns `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` otherwise. Customer never gets a silent wrong-vendor call.

### Mixing in customer code

```c
#include <alp/adc.h>
#include <alp/ext/alif/adc.h>

alp_adc_t *h = alp_adc_open(&cfg);
#if ALP_HAS(HW_OVERSAMPLE) && ALP_EXT_ALIF_ADC_AVAILABLE
    alp_alif_adc_set_oversampling(h, 8);
#endif
```

### `chips/<part>/` becomes strictly internal

The `feedback_portable_peripheral_api` memory tightens: app code MUST go through `<alp/X>` and MAY use `<alp/ext/<vendor>/X>` for vendor knobs. `chips/<part>/` is reserved for SDK backends and dedicated bridge demos.

---

## Section 4 — SW fallback tier

### Mechanism

Pure-software backends register at `priority = 0` (lowest) with `silicon_ref = "*"` (wildcard). The selector treats `"*"` as fall-through: considered only when no exact-match backend exists.

### File layout

```
src/backends/<class>/sw_fallback.c     /* one per subsystem with a fallback */
```

### Kconfig per subsystem

```kconfig
config ALP_SDK_ADC_SW_FALLBACK
    bool "Software ADC fallback (polled, no DMA)"
    default y if BOARD_NATIVE_SIM || ARCH_POSIX
    default n
    help
      Compiles the portable software ADC backend that registers at
      lowest priority.  Picks up when no hardware ADC backend is
      registered for the active SoC.
```

### What fallback means per class

| Class | Behaviour | ROM cost |
|---|---|---|
| ADC / DAC | Deterministic test pattern on `read()` | ~2 KB |
| I2C / SPI / UART | Loopback bus | ~1 KB |
| Inference | TFLM-micro CPU | ~120 KB |
| Storage | RAM-backed block device (volatile) | ~4 KB |
| Crypto / Security | MbedTLS software AES / SHA | ~40 KB |
| TMU | libm | ~0 |
| DSP | CMSIS-DSP scalar | ~20 KB |
| Camera / Display / GPU2D | No fallback — `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` | — |

### Documentation contract

Every `sw_fallback.c` file carries `@par Cost:` (ROM + RAM) and `@par Performance:` (vs HW) tags. CI gate validates tag presence. Summary table auto-generated into `docs/sw-fallback/index.md`.

### Customer signal that fallback is active

```c
if (strcmp(alp_adc_backend_vendor(h), "sw") == 0) {
    /* explicit: software fallback, not real hardware */
}
```

---

## Section 5 — Stub semantics + three error codes

### New codes in `<alp/peripheral.h>`

```c
typedef enum {
    ALP_OK = 0,
    /* ...existing codes... */

    ALP_ERR_NOT_PRESENT_ON_THIS_SOC = -64,  /* HW absent; paired with ALP_HAS()=0 */
    ALP_ERR_NOSUPPORT               = -22,  /* unchanged; config out of range */
    ALP_ERR_NOT_IMPLEMENTED         = -65,  /* planned, not yet wired */
} alp_error_t;
```

### Decision tree (inside every `alp_X_open()`)

```
                              alp_X_open(cfg)
                                    │
                  alp_backend_select("X", soc)
                                    │
              ┌─────────────────────┴─────────────────────┐
              │                                           │
       no match                                       match
              │                                           │
              ▼                                           ▼
ALP_ERR_NOT_PRESENT_ON_THIS_SOC               ops->open(cfg, &caps)
                                                          │
                            ┌─────────────────┬───────────┴──────────┐
                            ▼                 ▼                      ▼
                       ALP_OK +         ALP_ERR_NOSUPPORT       ALP_ERR_NOT_IMPLEMENTED
                       handle           (config bad)            (stub backend)
                       (caps filled)
```

### Stub backends become first-class citizens

A stub file (`src/backends/<class>/<vendor>_stub.c`) registers normally but its `ops->open` returns `ALP_ERR_NOT_IMPLEMENTED`. The file carries:

```c
/*
 * @brief Stub: NXP iMX9 ADC backend
 * @par Implementation status: NOT_IMPLEMENTED (planned: v0.8)
 * @par Tracking: github.com/alplabai/alp-sdk/issues/<N>
 */
```

CI gate: every stub backend must reference an open GitHub issue. Closing the issue triggers a stub-removal task in the maintainer's queue.

### Runtime `ALP_BACKEND_AVAILABLE()` macro

Distinct from PR #14's `ALP_HAS(<CAP>)` (which queries SoC-level capability macros). `ALP_BACKEND_AVAILABLE(<class>)` calls `alp_backend_count()` at runtime — it is NOT a compile-time constant and MUST NOT be used in `#if` directives. Use it in an ordinary `if(...)` statement. The two macros coexist:

```c
#if ALP_HAS(NPU_DRPAI)                  /* compile-time: SoC has the silicon block */
    if (ALP_BACKEND_AVAILABLE(inference)) {   /* runtime: backend is linked in */
        /* call alp_inference_open(...) — both gates satisfied */
    }
#endif
```

(The outer `#if` removes the call site on no-NPU silicon at compile time; the inner `if` guards against backend-not-linked at runtime.)

SoC gate true but backend missing at runtime → `alp_inference_open` returns `ALP_ERR_NOT_PRESENT_ON_THIS_SOC`. For compile-time pruning on no-backend builds, use `ALP_HAS(<CAP>)` alone. The double gate lets customers ship one binary across multiple SoMs in the same family.

---

## Section 6 — Migration order

The spec describes WHAT each migration delivers. Detailed HOW lives in per-slice plans created by `writing-plans` after this spec is approved.

### Slice 0 — Foundation (1 PR)

- `<alp/backend.h>` macro + selector
- `<alp/cap.h>` instance-cap types + getter contract
- `<alp/peripheral.h>` three new error codes
- `tests/regress/test_backend_section_visibility.c`
- Toy backend in `tests/unit/backend_registry/` proving end-to-end registration + selection + capability negotiation
- No subsystem migrated; no customer-facing behaviour changes

### Slice 1 — ADC pilot (1 PR)

- `src/backends/adc/{alif_e7,gd32_bridge,sw_fallback}.c` register
- Old `src/zephyr/peripheral_adc.c` deleted; `<alp/adc.h>` internals reroute through selector
- `alp_adc_capabilities()` getter shipped
- First vendor extension: `<alp/ext/alif/adc.h>` with `set_oversampling` + `set_trigger_source`
- `examples/adc-voltmeter` updated to use capability-gated DMA streaming
- `CONFIG_ALP_SDK_ADC_SW_FALLBACK` Kconfig added

### Slice 2 — Core peripherals (parallel, 6 PRs)

- I2C, SPI, UART, GPIO, PWM, I2S, CAN — each its own PR, near-mechanical mirror of the ADC pattern
- Vendor extensions added only where genuinely needed (e.g. `alp_alif_spi_set_quad_mode`)
- SW fallbacks for loopback-able classes (I2C/SPI/UART)
- Parallel-dispatchable since foundation is shared

### Slice 3 — Inference (1 PR)

- Replaces combinatorial `CONFIG_ALP_SDK_INFERENCE_*` ladder
- Backends: `tflm`, `ethos_u_aen`, `ethos_u_n93`, `drpai_v2n`, `deepx_dxm1`
- DRP-AI and DEEPX bodies (today `NOSUPPORT`) become `NOT_IMPLEMENTED` stubs tracked by GitHub issues
- Vendor extensions: `<alp/ext/renesas/inference.h>`, `<alp/ext/deepx/inference.h>`

### Slice 4 — Counter, QEnc, RTC, WDT, USB, Audio, BLE, IoT, Security, mproc, rpc, DSP, TMU (parallel, ~10 PRs)

- Mostly mechanical migrations
- Some have no vendor variation (RTC, WDT, BLE proxy) — single backend each, still register through the new mechanism for uniformity

### Slice 5 — Camera + ISP (1 PR)

- V2N RZ/V2N N44 ISP becomes a real backend (per maintainer correction 2026-05-21)
- `<alp/ext/renesas/camera.h>` exposes ISP-specific knobs (3A windows, gain curves)
- `<alp/ext/alif/camera.h>` for Mali-C55 on AEN E-series
- Camera capture base body lands (today fully stubbed) — expected longest slice
- Single-threaded since the ISP body is new code

### Slice 6 — Storage + inline-AES (1 PR)

- Real backends for Zephyr (Flash + littlefs), Yocto (block devices)
- Vendor extensions: `<alp/ext/alif/storage.h>` (OSPI SecAES), `<alp/ext/nxp/storage.h>` (FlexSPI OTFAD)
- Today's `storage_stub.c` deleted

### Slice 7 — Power (1 PR)

- Real `alp_power_request_sleep()` body via Zephyr `pm_policy_*`
- Vendor extensions: `<alp/ext/renesas/power.h>` (GD32 supervisor `CMD_POWER_MODE_SET`)
- Yocto path via `/sys/power/state`

### Slice 8 — Display + GPU2D (deferred post-v1.0)

- Spec records design; migration ships when underlying graphics stack lands
- Not on v1.0 critical path

### Sequencing rules

- Slice 0 blocks everything else.
- Slices 1 + 3 ship before Slice 2 fans out (validates pattern with real silicon variation first).
- Slices 4, 6, 7 can ship in parallel once 1 + 3 are in.
- Slice 5 is single-threaded.

### Estimated PR count

1 (foundation) + 1 (ADC) + 6 (core peripherals) + 1 (inference) + ~10 (slice 4) + 1 (camera) + 1 (storage) + 1 (power) = **~22 PRs**, sized to be reviewable in <30 minutes each.

---

## Cross-cutting concerns

### ABI status

`<alp/backend.h>` and `<alp/cap.h>` instance-cap additions are marked `[ABI-EXPERIMENTAL]` for v0.7. Promoted to `[ABI-STABLE]` after Slice 3 lands and the shape has been exercised against three vendor families (Alif + Renesas + NXP/DEEPX).

### Documentation

- `docs/architecture/backend-registry.md` — how the section pattern works, for new contributors
- `docs/extensions/index.md` — auto-generated catalogue of vendor extensions
- `docs/sw-fallback/index.md` — auto-generated cost/perf table
- `docs/abi/v0.7-snapshot.json` — regenerated to include the new public surface

### Testing

- `tests/unit/backend_registry/` — toy backend exercising registration, selection, capabilities, three error codes
- `tests/regress/test_backend_section_visibility.c` — guards against linker-script regressions
- Per-slice unit tests as subsystems migrate
- Existing twister + bitbake gates continue to cover end-to-end builds

### CI

- New gate: every stub backend file must reference an open GitHub issue (script: `scripts/check_stub_issues.py`)
- New gate: every vendor extension function must carry a `@par Supported silicon:` Doxygen tag
- New gate: every `sw_fallback.c` must carry `@par Cost:` and `@par Performance:` tags
- All three CI gates ship in Slice 0

---

## Open questions deferred to implementation

1. Exact `alp_capabilities_t` field set per subsystem — discovered during each slice's design.
2. Whether `alp_backend_select` should support runtime `silicon_ref` override (e.g. for simulators) — likely yes via env var; spec'd in Slice 0 plan.
3. Whether vendor extensions need their own ABI marker tier (`[ABI-VENDOR-EXTENSION]`) — decided during Slice 1 review.

## References

- Audit: chat transcript 2026-05-21, agent `a417a5f4d58eec5d9`
- Existing capability layer: PR #14 (`feat/capability-api`)
- Existing diagnostics: PR #15 (`feat/board-yaml-diagnostics`)
- Existing CLI: PR #16 (`feat/alp-cli-subcommands`)
- Memory: `feedback_portable_peripheral_api`, `project_simplification_unification_principle`, `project_v2n_no_dedicated_math_accelerator`, `feedback_portable_hw_offload_with_sw_fallback`
