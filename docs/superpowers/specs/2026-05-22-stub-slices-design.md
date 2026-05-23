# Stub Registry Slices (5 + 7 + 8) Design

**Date:** 2026-05-22
**Status:** Draft — pending implementation
**Owner:** alpCaner
**Spec depth:** Multi-subsystem mechanical scaffolding (one PR / one plan)
**Predecessor:** `docs/superpowers/specs/2026-05-21-backend-registry-design.md` Section 6 → Slices 5, 7, 8 (combined)
**Foundation:** PR #18 (`feat/backend-registry-adc-pilot`) — Slice 1 ADC pilot proved the pattern
**Branch:** `feat/backend-registry-stub-slices`

## Motivation

Three subsystems — **Camera (with ISP), Power, Display, GPU2D** — currently live as thin NOSUPPORT stubs in `src/zephyr/{camera_stub,gpu2d_zephyr,power_zephyr}.c` plus a placeholder-only `<alp/display.h>` with no implementation. None has real backends today.

This slice ships **registry scaffolding only** for all four: dispatcher + ops vtable + a single wildcard stub backend per subsystem. Each stub returns `ALP_ERR_NOT_IMPLEMENTED` and references a real GitHub issue tracking the future real-backend work. The slice deliberately ships **no real bodies** — that's the whole point. Once the scaffolding lands, real backends drop in atomically per their tracking issue without touching the public surface or the dispatcher.

The cost is small (~600 lines of mechanical scaffolding) and the unlock is large: every future "ship the real Alif Camera backend" PR becomes additive — one new `src/backends/camera/alif_e7.c`, register against a specific silicon_ref, done.

## Non-goals

- Any real backend implementation. Every stub returns `ALP_ERR_NOT_IMPLEMENTED`.
- ISP gets a separate header. Per the master-spec §3 audit, ISP knobs land on `alp_camera_config_t` and the existing `alp_camera_configure_isp` runtime call — no `<alp/isp.h>`. ISP feature work is tracked in issue #21 but does not happen in this slice.
- Migration of `v2n_power_mgmt.c` / `v2n_power_mgmt.h`. Those are V2N-internal supervisor protocol helpers, not peripheral implementations. Untouched.
- Display chip drivers under `include/alp/chips/` (ST7789, ILI9341, SSD1306, etc.). They're chip-level helpers for future Display real backends — out of scope.
- Yocto-side equivalents of any of these. Stay on the existing `src/yocto/` translation units (none of these subsystems have Yocto files today).

---

## Section 1 — File layout (per subsystem)

For each of `camera`, `power`, `display`, `gpu2d`:

```
src/
├── <class>_dispatch.c                    (new: dispatcher + handle pool)
└── backends/
    └── <class>/
        ├── <class>_ops.h                 (new: internal ops vtable + struct alp_<class>)
        └── zephyr_stub.c                 (new: ALP_ERR_NOT_IMPLEMENTED + GitHub issue link)
```

Plus a per-class ztest harness under `tests/unit/<class>_registry/` (4 harnesses total).

Files **deleted**:
- `src/zephyr/camera_stub.c`
- `src/zephyr/gpu2d_zephyr.c`
- `src/zephyr/power_zephyr.c`

`<alp/display.h>` had no implementation file to delete — this slice creates from scratch.

`<alp/camera.h>` already had an opaque `struct alp_camera` forward declaration — the new ops header carries the real layout. Same for Display and GPU2D. Power had a `struct alp_power` body inline in `power_zephyr.c`; moves to `power_ops.h`.

`src/zephyr/handles.h` and `src/zephyr/handles.c` are **not touched** for these subsystems — none of them had `alp_z_<class>_pool_*` plumbing there (camera/gpu2d/display were standalone, power used a static global, not a pool).

---

## Section 2 — Backend matrix

| Class    | Backends in this slice                          | Tracking issue(s)              |
|----------|-------------------------------------------------|--------------------------------|
| camera   | `zephyr_stub` (wildcard, priority 0)            | #20 Camera, #21 ISP            |
| power    | `zephyr_stub` (wildcard, priority 0)            | #22 Power                      |
| display  | `zephyr_stub` (wildcard, priority 0)            | #23 Display                    |
| gpu2d    | `zephyr_stub` (wildcard, priority 0)            | #24 GPU2D                      |

Single-backend selector behaviour: the wildcard stub matches every silicon_ref (including unknown), so `alp_<class>_open` returns NULL with `ALP_ERR_NOT_IMPLEMENTED` everywhere. Identical to today's behaviour, just routed through the registry.

When real backends land (separate slices), they register at priority 100 with specific silicon_refs (e.g. `alif:ensemble:e7`) and win the selector over the wildcard stub.

---

## Section 3 — Ops vtables (one per class)

Per-class internal header `src/backends/<class>/<class>_ops.h` follows the Slice 1/4a pattern exactly. The ops table fields map 1:1 to the public surface functions. Per-class surface inventory:

- **Camera (7 public)**: `open`, `start`, `stop`, `capture`, `release`, `configure_isp`, `close` → 7 ops entries.
- **Display (5 public)**: `open`, `get_caps`, `blit`, `clear`, `close` → 5 ops entries.
- **Power (4 public)**: `open`, `configure_wake_source`, `request_sleep`, `close` → 4 ops entries.
- **GPU2D (5 public)**: `open`, `fill_rect`, `blit`, `blend`, `close` → 5 ops entries.

`alp_<class>_backend_state_t` carries `void *be_data` + `const alp_<class>_ops_t *ops` plus per-class scalar fields the dispatcher reads on every call (e.g. Power's `wake_bitmap`).

`struct alp_<class>` (the handle struct opaque to customers) carries `state`, `backend`, `cached_caps`, `in_use` — same layout as Slice 1/4a.

---

## Section 4 — Stub backend file conventions

Each `src/backends/<class>/zephyr_stub.c` MUST carry:

1. SPDX header.
2. A `@par Tracking: github.com/alplabai/alp-sdk/issues/<N>` line in the top comment block (enforced by `scripts/check_stub_issues.py`).
3. All ops functions returning `ALP_ERR_NOT_IMPLEMENTED` (or NULL for `open`).
4. `ALP_BACKEND_REGISTER(<class>, zephyr_stub, { silicon_ref = "*", vendor = "stub", priority = 0, ... });`

Filename MUST end in `_stub.c` so the CI gate picks it up.

The Camera stub additionally references issue #21 (ISP) since the ISP knobs are part of the Camera surface contract.

`@par Cost:` and `@par Performance:` tags are **NOT** required on `_stub.c` files (`scripts/check_sw_fallback_tags.py` only scans files matching `sw_fallback.c`, not `*_stub.c`).

---

## Section 5 — Portable layer surface delta

Each public header gains exactly one new declaration: `alp_<class>_capabilities()`.

```c
const alp_capabilities_t *alp_camera_capabilities(const alp_camera_t *c);
const alp_capabilities_t *alp_power_capabilities(const alp_power_t *handle);
const alp_capabilities_t *alp_display_capabilities(const alp_display_t *d);
const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *handle);
```

No other public surface changes. Existing function signatures preserved byte-for-byte. Headers stay `[ABI-EXPERIMENTAL]` — they were already marked as such.

Since the stub `open` returns NULL today and continues to return NULL after this slice, the `alp_<class>_capabilities(NULL)` path is the only one customers can exercise — it returns NULL. The capabilities getter is real plumbing for the day real backends ship.

---

## Section 6 — Test scope

Per-class ztest harness, 6 cases each, mirroring Slice 4a's pattern:

1. `test_stub_picked_for_alif_e7` — `alp_backend_select("<class>", "alif:ensemble:e7")` returns the stub (vendor "stub", priority 0). This is the ONLY backend on this build, so it wins regardless of silicon_ref.
2. `test_stub_picked_for_unknown_silicon` — same selector behaviour for fictional silicon.
3. `test_select_returns_null_for_null_class` — input validation.
4. `test_select_returns_null_for_null_silicon_ref` — Slice 0 NULL-bug regression.
5. `test_<class>_open_returns_null` — public `open` returns NULL (stub semantics; the dispatcher routes through the stub's open which returns `ALP_ERR_NOT_IMPLEMENTED`).
6. `test_<class>_capabilities_returns_null_for_null_handle` — capability getter null safety.
7. `test_backend_count_for_<class>` — `alp_backend_count("<class>") == 1u` (only the stub).

For Camera + Display the `open` takes a config; tests pass a zeroed `alp_camera_config_t` / `alp_display_config_t` since the stub doesn't validate.

For Power + GPU2D the `open` takes no args.

---

## Section 7 — Subagent dispatch shape

Per the Slice 4a precedent: **one subagent = one subsystem, end to end**. Four sequential subagents:

1. Camera migration (references issues #20 + #21)
2. Power migration (references issue #22)
3. Display migration (references issue #23)
4. GPU2D migration (references issue #24)

Plus an orchestrator-driven Task 5: ABI snapshot regen + PR open.

Each subagent ships 6 commits:
1. `feat(<class>): declare alp_<class>_capabilities() public getter`
2. `feat(<class>): private backend ops vtable + handle layout`
3. `feat(<class>): class dispatcher + handle pool`
4. `feat(<class>): zephyr stub backend (issue #N tracking)`
5. `refactor(<class>): drop legacy src/zephyr/<file>.c` (skipped for Display — no legacy file)
6. `build(<class>): wire registry sources into cmake`
7. `test(<class>): unit-test harness for the registry`

That's 6–7 commits per subsystem × 4 subsystems = 25 commits + 1 ABI regen + 1 spec/plan commit = ~27 commits total.

---

## Cross-cutting concerns

### ABI impact

- Four new public functions: `alp_<class>_capabilities()` per class.
- Existing signatures unchanged.
- All four headers retain `[ABI-EXPERIMENTAL]`.
- ABI snapshot regen at the end of the slice.

### Migration risk

Minimal — the existing implementations are already NOSUPPORT/INVAL stubs. The new stubs preserve the same behaviour through the new dispatch path. No functional regression possible.

### Test coverage

Four new ztest harnesses (~28 cases total). No new vendor extensions, so `check_vendor_ext_tags.py` stays a no-op. No `sw_fallback.c` files (these are `_stub.c`), so `check_sw_fallback_tags.py` is also a no-op for new files. `check_stub_issues.py` becomes load-bearing on this slice — every new `_stub.c` carries an issue reference.

### Documentation

- `docs/extensions/index.md` unchanged (no new ext headers).
- No new docs files. Issue references in stub-file headers are the documentation surface.

---

## Open questions deferred to implementation

1. **`alp_power_t` handle model.** Today's `power_zephyr.c` uses a single static global; the registry pattern expects per-handle state. The dispatcher should use a 1-slot static pool to preserve "only one open() call succeeds at a time" semantics — or relax to a per-handle pool and document the behaviour change. Decided: 1-slot static pool. Behaviour preserved.
2. **`alp_gpu2d_t` handle model.** Same as Power — `gpu2d_zephyr.c` opens via a no-arg call and returns NULL today. Use a 1-slot static pool.
3. **Camera frame ownership in the stub.** `alp_camera_capture` returns a frame pointer in real implementations. The stub returns `ALP_ERR_NOT_IMPLEMENTED` without ever touching the out-param; matches current behaviour.

## References

- Architecture spec: `docs/superpowers/specs/2026-05-21-backend-registry-design.md`
- Slice 1 ADC (canonical template): `docs/superpowers/specs/2026-05-22-adc-registry-pilot-design.md`
- Slice 4a (most-recent template): `docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md`
- Foundation PR: #17 (Slice 0 registry)
- Stacks on: #18 (Slice 1 ADC), independent of #19 (Slice 4a)
- Tracking issues: #20 Camera, #21 ISP, #22 Power, #23 Display, #24 GPU2D
- Memory: `feedback_portable_hw_offload_with_sw_fallback`, `project_alif_ensemble_feature_audit_completed`
