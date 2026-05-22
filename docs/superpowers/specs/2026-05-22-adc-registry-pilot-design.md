# ADC Registry Pilot (Slice 1) Design

**Date:** 2026-05-22
**Status:** Draft — pending implementation
**Owner:** alpCaner
**Spec depth:** Single-subsystem migration (one PR / one plan)
**Predecessor:** `docs/superpowers/specs/2026-05-21-backend-registry-design.md` Section 6 → Slice 1
**Foundation:** PR #17 (`feat/backend-registry-foundation`)

## Motivation

Slice 0 (PR #17) ships the registry mechanism, capability types, and three CI gates — but no subsystem uses them yet. Slice 1 migrates ADC, the first real peripheral, to validate the pattern against actual silicon variation (Alif HAL on the Ensemble E-series, the GD32 supervisor MCU bridge on V2N) and to ship the first vendor-extension header. The migration is structural, not a rewrite: the existing Zephyr ADC driver-class wiring stays; what changes is how the SDK selects which backend to call.

## Non-goals

- DAC migration. DAC stays on the existing `#if`-ladder pattern until Slice 4 (mechanical migrations).
- DMA continuous-streaming mode. The public surface stays one-shot in Slice 1; streaming defers to Slice 1.x.
- Alif variants other than E7. E3 / E5 / E8 backends land in a v0.7.x follow-up — they're textual copies of the E7 backend with different `silicon_ref` strings and minor HAL-pack differences.
- Additional vendor extensions beyond `set_oversampling` + `set_trigger_source`. `set_reference` / `set_input_mode` defer.
- Migration of any other subsystem. Slice 2 onward is separate spec + plan documents.

---

## Section 1 — File layout + scope boundary

```
include/alp/
├── adc.h                 (modified: public surface unchanged + alp_adc_capabilities decl)
└── ext/
    └── alif/
        └── adc.h         (new: alp_alif_adc_set_oversampling, _set_trigger_source)

src/
├── adc_dispatch.c        (new: class dispatcher + raw→uV conversion + handle pool)
├── backends/
│   └── adc/
│       ├── adc_ops.h     (new: internal ops vtable header — private)
│       ├── alif_e7.c     (new: Alif Ensemble E7 backend + vendor-ext bodies)
│       ├── gd32_bridge.c (new: V2N ADC routed through GD32 supervisor MCU)
│       └── sw_fallback.c (new: native_sim test pattern)
├── zephyr/
│   ├── peripheral_adc.c  (REMOVED — ADC path moves out)
│   └── peripheral_dac.c  (new: DAC extracted from peripheral_adc.c, still on #if ladder)

tests/unit/adc_registry/  (new: ztest harness, eight cases)

examples/adc-voltmeter/   (touched: capability-gated teaching block)

zephyr/Kconfig            (modified: CONFIG_ALP_SDK_ADC_SW_FALLBACK)
zephyr/CMakeLists.txt     (modified: include new src/backends/adc/* + adc_dispatch.c)
```

**Key boundary decisions:**

- DAC code is extracted from `peripheral_adc.c` into its own `peripheral_dac.c` before `peripheral_adc.c` is removed. DAC stays on the old `#if`-ladder pattern. The new `src/backends/adc/` directory is genuinely ADC-only.
- `src/adc_dispatch.c` lives at `src/` top-level (next to `src/backend.c` and `src/cap_helpers.c` from Slice 0), not under `src/zephyr/`. The dispatcher is OS-agnostic; the backends bring in OS-specific glue.
- `gd32_bridge` keeps the V2N supervisor MCU routing per the `feedback_portable_peripheral_api` memory and the GD32 hybrid SPI+I2C contract.

---

## Section 2 — Backend ops vtable

Private internal header `src/backends/adc/adc_ops.h`:

```c
typedef struct alp_adc_backend_state {
    uint32_t                   reference_uv;
    uint16_t                   resolution_bits;
    const struct alp_adc_ops  *ops;
    void                      *be_data;
} alp_adc_backend_state_t;

typedef struct alp_adc_ops {
    alp_status_t (*open)(const alp_adc_config_t *cfg,
                         alp_adc_backend_state_t *state,
                         alp_capabilities_t *caps_out);
    alp_status_t (*read_raw)(alp_adc_backend_state_t *state, int32_t *raw_out);
    void         (*close)(alp_adc_backend_state_t *state);
} alp_adc_ops_t;
```

**`alp_adc_t` handle** (definition in `src/adc_dispatch.c`):

```c
struct alp_adc {
    alp_adc_backend_state_t  state;
    const alp_backend_t     *backend;
    alp_capabilities_t       cached_caps;
};
```

**Ops semantics:**

- `open` returns `ALP_OK` / error code AND fills `state->reference_uv` and `state->resolution_bits` so the portable layer can compute `read_uv` without walking the backend on every call.
- `read_raw` returns a signed `int32_t`. Single-ended SoCs return ≥ 0; differential SoCs use the full signed range.
- `close` may be NULL for stateless backends.

**Vendor extension reach-through:**

Vendor functions cast `h->backend->ops` to a vendor-specific struct that embeds `alp_adc_ops_t` as its first member (C's "first-member-aliasing" pattern):

```c
typedef struct alif_e7_adc_ops {
    alp_adc_ops_t base;
    alp_status_t (*set_oversampling)(alp_adc_t *h, uint16_t ratio);
    alp_status_t (*set_trigger_source)(alp_adc_t *h, uint8_t src);
} alif_e7_adc_ops_t;
```

The vendor function verifies `strcmp(h->backend->vendor, "alif") == 0` first and returns `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` if not. Only after the vendor check does it cast the ops pointer and call through.

---

## Section 3 — Portable layer

Public surface in `<alp/adc.h>` stays byte-for-byte the same, with ONE addition: `alp_adc_capabilities()`. Existing declarations (`alp_adc_open`, `alp_adc_read_raw`, `alp_adc_read_uv`, `alp_adc_close`) are unchanged.

```c
const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *h);
```

**Dispatcher implementation** (`src/adc_dispatch.c`):

```c
ALP_BACKEND_DEFINE_CLASS(adc);

static struct alp_adc _handle_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];
static uint8_t        _handle_in_use[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg)
{
    if (cfg == NULL) { _set_last_error(ALP_ERR_INVAL); return NULL; }
    const alp_backend_t *be = alp_backend_select("adc", ALP_SOC_REF_STR);
    if (be == NULL) { _set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC); return NULL; }
    const alp_adc_ops_t *ops = (const alp_adc_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        _set_last_error(ALP_ERR_NOT_IMPLEMENTED); return NULL;
    }
    struct alp_adc *h = _alloc_handle();
    if (h == NULL) { _set_last_error(ALP_ERR_NOMEM); return NULL; }
    h->backend = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    if (be->probe != NULL) {
        uint32_t refined = caps.flags;
        (void)be->probe(cfg->channel_id, &refined);
        caps.flags = refined;
    }
    alp_status_t rc = ops->open(cfg, &h->state, &caps);
    if (rc != ALP_OK) { _free_handle(h); _set_last_error(rc); return NULL; }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_adc_read_raw(alp_adc_t *h, int32_t *raw_out)
{
    if (h == NULL || raw_out == NULL) return ALP_ERR_INVAL;
    return h->state.ops->read_raw(&h->state, raw_out);
}

alp_status_t alp_adc_read_uv(alp_adc_t *h, int32_t *uv_out)
{
    if (h == NULL || uv_out == NULL) return ALP_ERR_INVAL;
    int32_t raw;
    alp_status_t rc = h->state.ops->read_raw(&h->state, &raw);
    if (rc != ALP_OK) return rc;
    const int64_t fs = (int64_t)((1u << h->state.resolution_bits) - 1u);
    *uv_out = (int32_t)((int64_t)raw * (int64_t)h->state.reference_uv / fs);
    return ALP_OK;
}

void alp_adc_close(alp_adc_t *h)
{
    if (h == NULL) return;
    if (h->state.ops->close != NULL) h->state.ops->close(&h->state);
    _free_handle(h);
}

const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}
```

**Error reporting:** existing `alp_last_error()` infrastructure is reused. `alp_adc_open` returns NULL on failure; customer calls `alp_last_error()` for the reason code.

**Conversion math:** raw → uV is centralised in `alp_adc_read_uv`. Every backend reports `raw` in the same signed convention. `reference_uv` and `resolution_bits` come from `state` filled at `open` time. 64-bit intermediate prevents overflow at 24-bit resolution × 3.3 V.

**Handle pool:** static array sized by `CONFIG_ALP_SDK_ADC_HANDLE_POOL` (default 8 — same Kconfig as the legacy `peripheral_adc.c` used).

---

## Section 4 — The three backends

### `src/backends/adc/alif_e7.c`

- Registers for `silicon_ref = "alif:ensemble:e7"`, `priority = 100`, `vendor = "alif"`.
- Backend body wraps Zephyr's `adc_*` driver class — same code path the existing `peripheral_adc.c` uses; migration is structural, not a rewrite.
- Per-instance state pool sized by `CONFIG_ALP_SDK_ADC_HANDLE_POOL`.
- Advertises `ALP_INSTANCE_CAP_HW_OVERSAMPLE | ALP_INSTANCE_CAP_HW_TRIGGER` in `base_caps`.
- `probe = NULL` in v0.7.0 — no per-instance refinement needed for the pilot.
- Vendor-ext function bodies live in the same file. Each verifies `strcmp(h->backend->vendor, "alif") == 0`, validates args, and writes to per-instance state.

### `src/backends/adc/gd32_bridge.c`

- Registers for `silicon_ref = "renesas:rzv2n:n44"`, `priority = 100`, `vendor = "renesas"` (SoC vendor, not the bridge chip).
- Routes through GD32G553 supervisor MCU using existing `<alp/chips/gd32g553.h>` internal SDK API (NOT customer-visible).
- Acquires supervisor mutex, sends `CMD_ADC_OPEN` / `CMD_ADC_READ`, releases.
- `base_caps = 0u` in Slice 1 — no HW oversample / trigger via the bridge. Could be raised in a follow-up once the bridge firmware grows support.
- No vendor-ext functions in Slice 1 (Renesas-specific knobs would land in `<alp/ext/renesas/adc.h>` in a separate slice).

### `src/backends/adc/sw_fallback.c`

- Registers for `silicon_ref = "*"`, `priority = 0`, `vendor = "sw"`.
- Returns a deterministic saw-wave: counter advances by 137 modulo 4096 per call. Lets `examples/` build on native_sim without HW.
- Carries `@par Cost:` and `@par Performance:` tags (enforced by Slice 0's `check_sw_fallback_tags.py`).
- Compiled only when `CONFIG_ALP_SDK_ADC_SW_FALLBACK=y`.

### Kconfig addition

```kconfig
config ALP_SDK_ADC_SW_FALLBACK
    bool "Software ADC fallback (polled, deterministic test pattern)"
    depends on ALP_SDK
    default y if BOARD_NATIVE_SIM || ARCH_POSIX
    default n
    help
      Compiles the software ADC backend as a last-resort wildcard.
      Picked only when no hardware backend matches the active SoC.
      Off by default on real silicon; default on for native_sim.
```

---

## Section 5 — Vendor extension surface + tests + example

### `include/alp/ext/alif/adc.h`

- Defines `ALP_EXT_ALIF_ADC_AVAILABLE 1` for compile-time discoverability.
- Declares `alp_alif_adc_trigger_t` enum (SOFTWARE, TIMER0–3, EXT_PIN).
- Declares `alp_alif_adc_set_oversampling(alp_adc_t *h, uint16_t ratio)` and `alp_alif_adc_set_trigger_source(alp_adc_t *h, alp_alif_adc_trigger_t src)`.
- Every public function carries `@par Supported silicon: alif:ensemble:e7` (enforced by Slice 0's `check_vendor_ext_tags.py`).
- File-level `[ABI-EXPERIMENTAL]` marker.

### Implementation

In `src/backends/adc/alif_e7.c`:

- `set_oversampling`: validates ratio is power-of-2 ≤ 256 (else `ALP_ERR_INVAL`), verifies Alif vendor (else `ALP_ERR_NOT_PRESENT_ON_THIS_SOC`), stores into per-instance state, applies to Zephyr ADC sequencer on next read.
- `set_trigger_source`: validates enum range, vendor check, writes to backend state.

### Tests — `tests/unit/adc_registry/`

Eight ztest cases under twister on native_sim:

1. `test_open_succeeds_on_supported_soc` — open with E7 selected via Kconfig, handle non-NULL, capabilities include `HW_OVERSAMPLE`.
2. `test_open_returns_not_present_on_no_backend` — silicon_ref override to a fictional ref, NULL handle, `alp_last_error() == ALP_ERR_NOT_PRESENT_ON_THIS_SOC`.
3. `test_open_returns_inval_on_null_config` — `alp_adc_open(NULL)` returns NULL, `alp_last_error() == ALP_ERR_INVAL`.
4. `test_read_raw_returns_value` — open against sw_fallback, asserts the saw wave advances by 137 per call modulo 4096.
5. `test_read_uv_converts_consistently` — fixture: raw=2048, ref=3300000, bits=12 → uV ≈ 1650366. Asserts the math.
6. `test_capabilities_pointer_lifetime` — pointer returned by `alp_adc_capabilities` remains valid through three reads.
7. `test_vendor_ext_rejects_wrong_vendor` — open against sw_fallback, call `alp_alif_adc_set_oversampling`, assert `ALP_ERR_NOT_PRESENT_ON_THIS_SOC`.
8. `test_vendor_ext_validates_ratio` — open against E7, `set_oversampling(7)` (not power-of-2), assert `ALP_ERR_INVAL`.

**Test-only silicon_ref override:** the unit test compiles a shim that calls `alp_backend_select("adc", "<test ref>")` directly, bypassing `ALP_SOC_REF_STR`. Avoids per-test Kconfig fragments.

### `examples/adc-voltmeter` update

The existing example reads a channel and prints µV. Add a capability-gated teaching block:

```c
alp_adc_t *h = alp_adc_open(&cfg);
const alp_capabilities_t *caps = alp_adc_capabilities(h);

if (alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_OVERSAMPLE)) {
#if ALP_EXT_ALIF_ADC_AVAILABLE
    /* Vendor knob — only on Alif. The `#if` strips the call site on
     * non-Alif builds; the `if` gate handles per-instance refinement. */
    alp_alif_adc_set_oversampling(h, 8);
#endif
}
```

Builds on every SoM. Customer reading the example learns both gating mechanisms (runtime `if (alp_capabilities_has(...))` and compile-time `#if ALP_EXT_*_AVAILABLE`) side by side.

### CI gates (already in place from Slice 0)

- `check_vendor_ext_tags.py` validates `@par Supported silicon:` on `<alp/ext/alif/adc.h>` — header carries the tags.
- `check_sw_fallback_tags.py` validates `@par Cost:` + `@par Performance:` on `src/backends/adc/sw_fallback.c` — file carries them.
- `check_stub_issues.py` — no stub backends in this slice, no-op.

---

## Cross-cutting concerns

### ABI impact

- One new public function (`alp_adc_capabilities`) in `<alp/adc.h>`.
- One new public header (`<alp/ext/alif/adc.h>`) with two functions + one enum.
- ABI snapshot refreshed; `[ABI-EXPERIMENTAL]` markers on the new surface.
- Existing `alp_adc_open` / `alp_adc_read_raw` / `alp_adc_read_uv` / `alp_adc_close` signatures unchanged.

### Migration risk

- `peripheral_adc.c` deletion removes 888 lines of working code. The risk is functional regression on the Alif Zephyr path. Mitigation: `alif_e7.c` is a structural lift of the existing Zephyr-class wiring — same `adc_channel_setup` / `adc_read` calls, same Kconfig-driven behaviour — with the dispatch surrounding it replaced.
- The Zephyr `adc` driver class behavior is unchanged. Real-hardware regression risk concentrates in the GD32 bridge backend (the V2N path), which keeps its existing supervisor-MCU command set verbatim.
- DAC extraction to `peripheral_dac.c` is mechanical (DAC code is independently structured in the existing file — separate functions, separate handle pool). Risk: stray references between ADC and DAC sections of `peripheral_adc.c`. Plan-stage step: `grep -E "_dac_|_adc_" peripheral_adc.c` shows cross-references; resolve before extraction.

### Test coverage

- Eight ztest cases cover the dispatcher decision tree, error codes, capability propagation, and vendor-ext vendor-check.
- Existing `tests/scripts/` python tests for the CI gates remain green (the gates are no-ops on Slice 0's empty inputs; in Slice 1 they have content to enforce).
- Native_sim path validates the sw_fallback registration. Real-silicon validation comes from twister on Alif boards in CI.

### Documentation

- `docs/architecture/backend-registry.md` (from Slice 0) covers the contributor guide; no update needed.
- `docs/extensions/index.md` auto-generated from header front-matter — first entry lands here for `alp/ext/alif/adc.h`.
- `docs/sw-fallback/index.md` auto-generated cost table — first entry for `adc/sw_fallback.c`.
- Both auto-gen scripts ship as small additions to the Slice 1 plan (one task each).

---

## Open questions deferred to implementation

1. `alif_e7.c` vendor-ext function bodies: whether `set_oversampling` re-opens the Zephyr ADC sequence (slow but always-correct) or queues the ratio for the next `read_raw` (fast but ordering-sensitive). Decided during the plan's TDD step that exercises both call orders.
2. GD32 bridge ABI: the existing supervisor MCU command set is in `firmware/gd32-bridge/protocol_vectors.txt`. The plan should verify the `CMD_ADC_*` opcodes haven't shifted since the legacy `peripheral_adc.c` was written.
3. Whether `alp_adc_capabilities` returns a const pointer or a copy. Spec says const pointer; if the team prefers value semantics for ABI stability, swap during implementation. Cost is one struct copy per call.

## References

- Architecture spec: `docs/superpowers/specs/2026-05-21-backend-registry-design.md` Section 6 → Slice 1
- Foundation PR: #17 (`feat/backend-registry-foundation`)
- Existing ADC implementation: `src/zephyr/peripheral_adc.c` (888 lines, to be replaced)
- Existing example: `examples/adc-voltmeter/src/main.c`
- Memory: `feedback_portable_peripheral_api`, `project_gd32_bridge_hybrid_spi_i2c`, `feedback_portable_hw_offload_with_sw_fallback`
