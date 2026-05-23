# Simple Peripherals Registry Migration (Slice 4a) Design

**Date:** 2026-05-22
**Status:** Draft — pending implementation
**Owner:** alpCaner
**Spec depth:** Multi-peripheral mechanical migration (one PR / one plan)
**Predecessor:** `docs/superpowers/specs/2026-05-21-backend-registry-design.md` Section 6 → Slice 4a
**Foundation:** PR #18 (`feat/backend-registry-adc-pilot`) — Slice 1 ADC pilot proved the pattern
**Branch:** `feat/backend-registry-simple-peripherals`

## Motivation

Slice 1 (PR #18) proved the registry pattern against real silicon variation by migrating ADC. Slice 4a takes the four simplest remaining peripherals — **RTC, WDT, Counter, QEnc** — onto the same pattern in a single PR. These four are:

- **Self-contained.** Each `src/zephyr/peripheral_<x>.c` is 100–210 lines.
- **Mostly-portable.** Three (RTC, WDT, Counter) use Zephyr's driver class verbatim on every SoC we ship; one (QEnc) uses Zephyr's sensor API.
- **Already bridge-aware.** Counter and QEnc carry a V2N supervisor-MCU branch today (`#if CONFIG_ALP_SDK_V2N_SUPERVISOR`); the registry naturally splits that into a separate backend.
- **Vendor-extension-free.** Every per-peripheral feature is reachable through Zephyr's portable driver-class config. The audit rule from §3 of the master spec keeps `include/alp/ext/` empty for this slice.

Migrating four peripherals in one PR is appropriate here because each one is a tight structural lift of Slice 1, the failure modes overlap, and stacked PR review fatigue is real. Subagent dispatch fans the work out — one subagent per peripheral — so context never compounds beyond a single peripheral.

## Non-goals

- Vendor extensions. None of the four pass the master-spec §3 audit. If a feature needs vendor-specific knobs in a follow-up slice (e.g. Counter PPI on Nordic, RTC alarm callbacks), it ships as a `<alp/ext/<vendor>/<class>.h>` add-on then.
- Streaming variants. Counter has no streaming surface; the other three are inherently one-shot.
- ADC stream migration (still on the legacy path inside `peripheral_adc.c` per Slice 1).
- DAC migration. Stays on the `#if`-ladder pattern through Slice 4b.
- Comms (USB/BLE/IoT), IPC, Compute, Storage. Separate slices.

---

## Section 1 — File layout (per peripheral)

For each of `rtc`, `wdt`, `counter`, `qenc`:

```
src/
├── <class>_dispatch.c                    (new: dispatcher + handle pool)
└── backends/
    └── <class>/
        ├── <class>_ops.h                 (new: internal ops vtable + struct alp_<class>)
        ├── zephyr_drv.c                  (new: portable Zephyr driver-class backend)
        ├── sw_fallback.c                 (new: deterministic native_sim test backend)
        └── gd32_bridge.c                 (Counter + QEnc ONLY: V2N supervisor route)
```

Plus a single per-class ztest harness under `tests/unit/<class>_registry/`.

The current `src/zephyr/peripheral_<class>.c` files are **deleted** at the end of each migration. The `struct alp_<class>` definitions move from `src/zephyr/handles.h` into the per-class `<class>_ops.h`. The `alp_z_<class>_pool_*` helpers are dropped (dispatchers own their own pools).

---

## Section 2 — Backend matrix

| Class    | zephyr_drv (priority 100, wildcard) | gd32_bridge (priority 100, renesas:rzv2n:n44) | sw_fallback (priority 0, wildcard) |
|----------|------------------------------------:|----------------------------------------------:|-----------------------------------:|
| rtc      | yes                                 | no                                             | yes                                |
| wdt      | yes                                 | no                                             | yes                                |
| counter  | yes (gated `!V2N_SUPERVISOR` build) | yes                                            | yes                                |
| qenc     | yes (gated `!V2N_SUPERVISOR` build) | yes                                            | yes                                |

Selector behaviour:
- `silicon_ref == "renesas:rzv2n:n44"` and Counter/QEnc class → `gd32_bridge` wins (specific match beats wildcard).
- Any other `silicon_ref` with a registered `zephyr_drv` → `zephyr_drv` wins (priority 100 > 0).
- No `zephyr_drv` registered for this build (e.g. trimmed test build) → `sw_fallback` wins.

This mirrors the Slice 1 ADC selector matrix (`alif_e7` / `gd32_bridge` / `sw_fallback`), with the only delta being that the Alif/Renesas split on ADC collapses to a single `zephyr_drv` here because Zephyr's RTC/WDT/Counter/QDEC driver classes are portable across both SoCs we ship.

---

## Section 3 — Ops vtables (one per class)

Per-class internal header `src/backends/<class>/<class>_ops.h` follows the Slice 1 ADC pattern exactly. RTC example:

```c
typedef struct alp_rtc_ops {
    alp_status_t (*open)(uint32_t rtc_id,
                         alp_rtc_backend_state_t *state,
                         alp_capabilities_t *caps_out);
    alp_status_t (*set_time)(alp_rtc_backend_state_t *state,
                             const alp_rtc_time_t *time);
    alp_status_t (*get_time)(alp_rtc_backend_state_t *state,
                             alp_rtc_time_t *time);
    void         (*close)(alp_rtc_backend_state_t *state);
} alp_rtc_ops_t;
```

The handle struct (`struct alp_rtc`, etc.) lives in the same per-class ops header — same idiom as `adc_ops.h` after the Slice 1 Task 6 refactor.

**No vendor reach-through structs.** Slice 4a ships no vendor extensions, so the ops table is the base ops only. No first-member aliasing needed.

---

## Section 4 — Portable layer surface delta

Each public header gains exactly one new declaration: `alp_<class>_capabilities()`.

```c
const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *rtc);
const alp_capabilities_t *alp_wdt_capabilities(const alp_wdt_t *wdt);
const alp_capabilities_t *alp_counter_capabilities(const alp_counter_t *counter);
const alp_capabilities_t *alp_qenc_capabilities(const alp_qenc_t *enc);
```

No other public surface changes. Existing function signatures are byte-for-byte preserved.

Capabilities are advertised per the master spec §2 instance-cap layer. For Slice 4a:
- RTC: `0u` (no portable per-instance caps yet — alarms wait for follow-up).
- WDT: `0u` (`ALP_INSTANCE_CAP_HW_TIMEOUT` lands when we surface windowed mode portably).
- Counter: `ALP_INSTANCE_CAP_HW_ALARM` when the backend supports `alp_counter_set_alarm` (Zephyr does; bridge doesn't).
- QEnc: `0u` (`ALP_INSTANCE_CAP_HW_INDEX` defers).

---

## Section 5 — Vendor-extension audit (per master spec §3)

Each peripheral's per-SoC feature inventory was walked against the audit rule:

| Feature considered           | Class    | Portable equivalent                                | Decision |
|------------------------------|----------|----------------------------------------------------|----------|
| RTC calibration ppm trim     | rtc      | Zephyr's `rtc_set_calibration()` driver-class call | PORTABLE (add to `alp_rtc_config_t` in a follow-up) |
| RTC alarm callback           | rtc      | Zephyr's `rtc_alarm_set_callback` driver-class API | PORTABLE (deferred to add `alp_rtc_set_alarm`) |
| WDT windowed mode (min/max)  | wdt      | Existing `alp_wdt_config_t` has `timeout_ms`; add `window_min_ms` | PORTABLE (deferred) |
| Counter top/wrap value       | counter  | Zephyr's `counter_set_top_value` driver-class API  | PORTABLE (deferred) |
| QEnc decoder filter divider  | qenc     | Zephyr QDEC binding's `steps` DT property          | PORTABLE via devicetree |

**Conclusion: zero vendor extensions in Slice 4a.** `include/alp/ext/` is untouched. The decisions above are explicitly captured in the plan's commit messages and the per-class spec comments.

---

## Section 6 — Test scope

Per the writing-plans handoff: **one ztest harness per dispatcher**. Four harnesses under `tests/unit/{rtc,wdt,counter,qenc}_registry/`. Each follows the Slice 1 `tests/unit/adc_registry/` template:

Per harness, six cases minimum:
1. `test_realhw_picked_over_sw_on_<ref>` — Zephyr backend wins when its silicon_ref matches.
2. `test_sw_fallback_picked_for_unknown_silicon` — wildcard sw fallback for fictional ref.
3. `test_select_returns_null_for_null_class` — input validation.
4. `test_select_returns_null_for_null_silicon_ref` — Slice 0 NULL-bug regression.
5. `test_<class>_open_inval_on_null_config` — public API validation.
6. `test_<class>_capabilities_returns_null_for_null_handle` — capability getter null safety.

For Counter and QEnc, two extra cases verify the bridge backend is preferred over the Zephyr driver when `silicon_ref == "renesas:rzv2n:n44"`.

All harnesses pin `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y` to drive the dispatcher into the Zephyr-driver-class path on native_sim. (Same pattern Slice 1 used.)

---

## Section 7 — Subagent dispatch shape

Per the prior-session lesson on Slice 2: **one subagent = one peripheral, end to end**. Four sequential subagents, each owning ~6 commits:

1. RTC migration
2. WDT migration
3. Counter migration (+ bridge backend)
4. QEnc migration (+ bridge backend)

Plus a fifth orchestrator-driven task at the end:
5. ABI snapshot regeneration + PR open.

The orchestrator integrates between subagents (verifies commits, runs CI gates locally, refreshes context) and only fires the next subagent after the prior one's work is committed and clean. If a subagent crashes mid-task, its uncommitted state is rolled back via `git reset --hard HEAD` and that peripheral is retried from scratch with a fresh subagent. No salvage of partial state.

Each subagent brief points at this spec, the Slice 1 ADC plan (`docs/superpowers/plans/2026-05-22-adc-registry-pilot.md`), and the literal commits `bfc6543..HEAD of feat/backend-registry-adc-pilot` as the structural template.

---

## Cross-cutting concerns

### ABI impact

- Four new public functions: `alp_rtc_capabilities`, `alp_wdt_capabilities`, `alp_counter_capabilities`, `alp_qenc_capabilities`.
- Zero new public headers (all four declarations go into existing `<alp/rtc.h>`, `<alp/wdt.h>`, `<alp/counter.h>` — qenc is in counter.h).
- Existing function signatures unchanged.
- ABI snapshot refreshed at the end of the slice; no `[ABI-EXPERIMENTAL]` markers needed (the new getters follow the same `[ABI-STABLE]` marker the headers already carry).

### Migration risk

- ~554 lines of working `peripheral_<x>.c` code is replaced by ~700 lines of registry-pattern code (more boilerplate, same behaviour). Functional regression risk concentrates in the Counter + QEnc bridge paths, which are kept verbatim from `peripheral_counter.c` / `peripheral_qenc.c`.
- The `struct alp_<class>` migration out of `handles.h` is the most invasive piece. Any code in `src/zephyr/` outside the four peripheral files that reaches into `handles.h` for these structs needs to follow the move. Subagents run `grep -nE "struct alp_(rtc|wdt|counter|qenc)" src/ zephyr/` before declaring done.

### Test coverage

- Four new ztest harnesses (~24 cases total) under `tests/unit/<class>_registry/`.
- Existing CI gates (`check_sw_fallback_tags.py`, `check_vendor_ext_tags.py`, `check_stub_issues.py`) stay green — no new vendor extensions, every new `sw_fallback.c` carries the required `@par Cost:` and `@par Performance:` tags, no stub backends.

### Documentation

- `docs/sw-fallback/index.md` auto-gen picks up four new entries (rtc, wdt, counter, qenc).
- `docs/extensions/index.md` unchanged (no new ext headers).
- `docs/architecture/backend-registry.md` from Slice 0 covers the contributor guide; no update needed.

---

## Open questions deferred to implementation

1. **Counter alarm capability on V2N bridge.** The existing `peripheral_counter.c` returns `ALP_ERR_NOSUPPORT` for `alp_counter_set_alarm` on the bridge path. The new `gd32_bridge.c` keeps this. Whether `alp_counter_capabilities()` should clear `ALP_INSTANCE_CAP_HW_ALARM` for bridge handles via the registry's `probe` callback is decided in the implementation step that writes the bridge backend's `open()`.
2. **QEnc resolution semantics.** The existing implementation accumulates `sensor_value.val1` (integer degrees). The new backend keeps this verbatim. Whether to add a portable `alp_qenc_get_pulses` follow-up is a v0.7.x design call.
3. **sw_fallback semantics for RTC.** The fake clock either ticks at process wall-time (matches real RTC) or returns a frozen fixture (matches test determinism). Decided in the writing-tests step — default is frozen 2026-01-01 00:00:00 with a per-call increment.

## References

- Architecture spec: `docs/superpowers/specs/2026-05-21-backend-registry-design.md` Section 6 → Slice 4a
- Slice 1 design (canonical template): `docs/superpowers/specs/2026-05-22-adc-registry-pilot-design.md`
- Slice 1 plan (literal task structure): `docs/superpowers/plans/2026-05-22-adc-registry-pilot.md`
- Foundation PR: #17 (`feat/backend-registry-foundation`)
- Slice 1 PR (depends-on): #18 (`feat/backend-registry-adc-pilot`)
- Existing implementations being replaced:
  - `src/zephyr/peripheral_rtc.c` (107 lines)
  - `src/zephyr/peripheral_wdt.c` (109 lines)
  - `src/zephyr/peripheral_counter.c` (206 lines)
  - `src/zephyr/peripheral_qenc.c` (136 lines)
- Memory: `feedback_portable_peripheral_api`, `project_gd32_bridge_hybrid_spi_i2c`, `feedback_portable_hw_offload_with_sw_fallback`
