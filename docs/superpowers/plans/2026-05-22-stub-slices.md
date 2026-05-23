# Stub Registry Slices (5 + 7 + 8) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: superpowers:subagent-driven-development to dispatch ONE subagent per subsystem.

**Goal:** Install registry scaffolding (dispatcher + ops vtable + single stub backend + test harness) for Camera, Power, Display, GPU2D. NO real bodies. Each stub references a real GitHub tracking issue.

**Architecture:** Same shape as Slice 1 ADC and Slice 4a — one class dispatcher per subsystem at `src/<class>_dispatch.c`, internal ops vtable at `src/backends/<class>/<class>_ops.h`, one stub backend at `src/backends/<class>/zephyr_stub.c` (wildcard, priority 0, vendor "stub", returns `ALP_ERR_NOT_IMPLEMENTED`). Test harness at `tests/unit/<class>_registry/`.

**Tech Stack:** C17 (ztest on native_sim), Kconfig (no new symbols — stubs are unconditional), `scripts/check_stub_issues.py` CI gate.

**Spec:** `docs/superpowers/specs/2026-05-22-stub-slices-design.md`
**Structural template:** `docs/superpowers/plans/2026-05-22-simple-peripherals-slice4a.md` Task 1 (RTC) for the literal commit cadence; this slice drops the bridge backend + sw_fallback + Kconfig steps since stubs don't need them.
**Branch:** `feat/backend-registry-stub-slices` (already created off `feat/backend-registry-adc-pilot`).
**Tracking issues:** #20 Camera, #21 ISP, #22 Power, #23 Display, #24 GPU2D (already opened).

---

## Pre-flight

- [ ] **Step 1: Verify branch**

```bash
git branch --show-current   # feat/backend-registry-stub-slices
git status --porcelain      # empty
```

- [ ] **Step 2: Commit the spec + plan**

```bash
git add docs/superpowers/specs/2026-05-22-stub-slices-design.md \
        docs/superpowers/plans/2026-05-22-stub-slices.md
git commit -m "docs(spec): slice 5+7+8 design + plan for stub-on-registry scaffolding"
```

---

## Task 1 — Subagent: Camera migration (references issues #20 + #21)

**Files:**
- Modify: `include/alp/camera.h` (add `alp_camera_capabilities` decl)
- Create: `src/backends/camera/camera_ops.h`, `src/camera_dispatch.c`, `src/backends/camera/zephyr_stub.c`
- Delete: `src/zephyr/camera_stub.c`
- Modify: `zephyr/CMakeLists.txt`
- Create: `tests/unit/camera_registry/{CMakeLists.txt, prj.conf, testcase.yaml, src/test_camera_registry.c}`

### Subagent brief (paste verbatim)

> You are migrating the Camera peripheral to the backend registry pattern with a STUB backend (no real implementation). Template is Slice 4a RTC (`docs/superpowers/plans/2026-05-22-simple-peripherals-slice4a.md` Task 1) and the just-merged RTC commits on `feat/backend-registry-adc-pilot`'s child slice 4a branch. **DO NOT switch branches** — work on `feat/backend-registry-stub-slices`, verify via `git branch --show-current`.
>
> **Critical:** the stub file MUST be named `zephyr_stub.c` (ends in `_stub.c`) and MUST carry a `@par Tracking: github.com/alplabai/alp-sdk/issues/20` line in its top comment block — and ALSO reference issue #21 (ISP) since ISP knobs are part of the Camera surface contract. The CI gate `scripts/check_stub_issues.py` walks `src/backends/**/*_stub.c` and fails if the tracking line is missing.
>
> **DO NOT touch:** any other subsystem; `src/zephyr/handles.h` or `handles.c` (camera had no pool plumbing there); `include/alp/boards/alp_e1m_evk_routes.h`; `include/alp/soc_caps.h` if CRLF-only diff. No `Co-Authored-By: Claude` footer.
>
> Public surface (7 functions, do not change signatures):
> ```
> alp_camera_t *alp_camera_open(const alp_camera_config_t *cfg);
> alp_status_t  alp_camera_start(alp_camera_t *c);
> alp_status_t  alp_camera_stop(alp_camera_t *c);
> alp_status_t  alp_camera_capture(alp_camera_t *c, alp_camera_frame_t *frame_out, uint32_t timeout_ms);
> alp_status_t  alp_camera_release(alp_camera_t *c, alp_camera_frame_t *frame);
> alp_status_t  alp_camera_configure_isp(alp_camera_t *camera, const alp_camera_isp_config_t *isp);
> void          alp_camera_close(alp_camera_t *c);
> ```
> Add one new declaration: `const alp_capabilities_t *alp_camera_capabilities(const alp_camera_t *c);`
>
> Read the EXACT signature of `alp_camera_capture` from `include/alp/camera.h` before you write the ops vtable — the arg list above is illustrative; the header is authoritative.
>
> **Six commits, one per step:**
>
> **Commit 1 — `feat(camera): declare alp_camera_capabilities() public getter`**: append decl + `#include <alp/cap_instance.h>` to `include/alp/camera.h`.
>
> **Commit 2 — `feat(camera): private backend ops vtable + handle layout`**: create `src/backends/camera/camera_ops.h` with `alp_camera_ops_t` (7 function pointers matching the public surface), `alp_camera_backend_state_t` (`void *be_data`, `const alp_camera_ops_t *ops`, plus any per-instance scalars you need — for the stub, none are needed), and `struct alp_camera { state, backend, cached_caps, in_use }`.
>
> **Commit 3 — `feat(camera): class dispatcher + handle pool`**: create `src/camera_dispatch.c`. Mirror `src/rtc_dispatch.c` shape with the 7-function surface. `ALP_BACKEND_DEFINE_CLASS(camera)`. Pool size: `CONFIG_ALP_SDK_MAX_CAMERA_HANDLES` or default 2. Reuse `alp_z_set_last_error` / `alp_z_clear_last_error` via `extern` decls.
>
> **Commit 4 — `feat(camera): zephyr stub backend (issues #20, #21 tracking)`**: create `src/backends/camera/zephyr_stub.c`. Top comment block MUST include:
> ```
>  * @par Tracking: github.com/alplabai/alp-sdk/issues/20
>  * @par Tracking: github.com/alplabai/alp-sdk/issues/21
> ```
> All ops functions return `ALP_ERR_NOT_IMPLEMENTED`. `open` returns `ALP_ERR_NOT_IMPLEMENTED` from the ops table (the dispatcher will convert to NULL + last_error). Register:
> ```c
> ALP_BACKEND_REGISTER(camera, zephyr_stub, {
>     .silicon_ref = "*",
>     .vendor      = "stub",
>     .base_caps   = 0u,
>     .priority    = 0,
>     .ops         = &_ops,
>     .probe       = NULL,
> });
> ```
>
> **Commit 5 — `refactor(camera): drop legacy src/zephyr/camera_stub.c`**: `git rm src/zephyr/camera_stub.c`. No handles.h changes (camera had no pool there).
>
> **Commit 6 — `build(camera): wire registry sources into cmake`**: in `zephyr/CMakeLists.txt`, replace any `src/zephyr/camera_stub.c` reference with:
> ```cmake
> zephyr_library_sources(
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/camera_dispatch.c
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/camera/zephyr_stub.c)
> ```
> No Kconfig changes (stubs are always-on; no `*_SW_FALLBACK` symbol needed since this isn't a fallback).
>
> **Commit 7 — `test(camera): unit-test harness for the camera registry`**: create `tests/unit/camera_registry/` mirroring `tests/unit/rtc_registry/`. Suite name `alp_camera_registry`. Include via `#include "../../../../src/backends/camera/camera_ops.h"`. Seven cases per spec §6 — note that for Camera the stub is the ONLY backend, so test 1 (`test_stub_picked_for_alif_e7`) asserts vendor "stub" priority 0; test 7 (`test_backend_count_for_camera`) asserts count == 1u.
>
> **Final reporting checklist:**
> - 7 commits (matches RTC's 8 minus the Kconfig commit).
> - `git status --porcelain` empty.
> - `python3 scripts/check_stub_issues.py` exits 0 (validates the `@par Tracking:` line).
> - `python3 scripts/check_sw_fallback_tags.py` exits 0 (no `sw_fallback.c` files in this slice).
> - `python3 scripts/check_vendor_ext_tags.py` exits 0.
> - `python3 scripts/abi_snapshot.py --version v0.5 --output /tmp/abi-camera-smoke.json` exits 0.
> - `grep -rnE "camera_stub\.c" src/ zephyr/` returns only documentation / yocto references (the legacy Zephyr file is gone).
>
> Report: status, commit SHAs, files changed, deviations.

---

## Task 2 — Subagent: Power migration (references issue #22)

**Files:** as Task 1 with `power` substituted; legacy file `src/zephyr/power_zephyr.c` is deleted.

### Subagent brief (paste verbatim)

> Migrate Power to the registry stub pattern. Template: Camera (Task 1, just landed). DO NOT switch branches.
>
> Public surface (4 functions):
> ```
> alp_power_t *alp_power_open(void);
> alp_status_t alp_power_configure_wake_source(alp_power_t *handle, uint32_t wake_bitmap);
> alp_status_t alp_power_request_sleep(alp_power_t *handle, alp_power_mode_t mode,
>                                       uint32_t wake_after_ms, alp_power_wake_info_t *info);
> void         alp_power_close(alp_power_t *handle);
> ```
> Add: `const alp_capabilities_t *alp_power_capabilities(const alp_power_t *handle);`
>
> **Single-slot handle pool semantics:** `alp_power_open` historically returned a static global pointer. The new dispatcher uses a 1-slot static pool (size `CONFIG_ALP_SDK_MAX_POWER_HANDLES` or default 1) to preserve "only one open() call active at a time" behaviour. Document this in commit 3's message.
>
> **Stub commit 4 message** references issue #22:
> ```
>  * @par Tracking: github.com/alplabai/alp-sdk/issues/22
> ```
> Stub `open` returns `ALP_ERR_NOT_IMPLEMENTED`; `configure_wake_source` validates non-null handle and returns OK (the stub silently accepts wake-source config since stub `request_sleep` will fail anyway); `request_sleep` returns `ALP_ERR_NOT_IMPLEMENTED`; `close` is a no-op.
>
> **Commit 5 — `refactor(power): drop legacy src/zephyr/power_zephyr.c`**: `git rm src/zephyr/power_zephyr.c`. **DO NOT touch `src/zephyr/v2n_power_mgmt.{c,h}`** — those are V2N supervisor protocol helpers, not peripheral implementations. They stay exactly as they are.
>
> **Commit 6 — `build(power): wire registry sources into cmake`**: replace the `src/zephyr/power_zephyr.c` reference in `zephyr/CMakeLists.txt` with the dispatcher + stub. Leave the `v2n_power_mgmt.c` references in CMakeLists alone.
>
> Same 7-commit cadence as Camera. Test harness file: `tests/unit/power_registry/`. The `alp_power_open` test takes no args. Sixth-case test uses NULL handle: `test_power_capabilities_returns_null_for_null_handle`.
>
> Report 7 commit SHAs + verification grep + script gates.

---

## Task 3 — Subagent: Display migration (references issue #23)

**Files:** as Task 1 with `display` substituted; **no legacy file to delete** (no `display_zephyr.c` exists today).

### Subagent brief (paste verbatim)

> Migrate Display to the registry stub pattern. Template: Camera/Power tasks just landed. DO NOT switch branches.
>
> Public surface (5 functions):
> ```
> alp_display_t *alp_display_open(const alp_display_config_t *cfg);
> alp_status_t   alp_display_get_caps(alp_display_t *d, alp_display_caps_t *out);
> alp_status_t   alp_display_blit(alp_display_t *d, /* args from header */);
> alp_status_t   alp_display_clear(alp_display_t *d);
> void           alp_display_close(alp_display_t *d);
> ```
> Read `include/alp/display.h` for the exact `alp_display_blit` signature.
>
> Add: `const alp_capabilities_t *alp_display_capabilities(const alp_display_t *d);`
>
> **No legacy file to delete.** This slice creates Display's first implementation from scratch.
>
> Commits:
> 1. `feat(display): declare alp_display_capabilities() public getter`
> 2. `feat(display): private backend ops vtable + handle layout`
> 3. `feat(display): class dispatcher + handle pool`
> 4. `feat(display): zephyr stub backend (issue #23 tracking)` — top-of-file `@par Tracking: github.com/alplabai/alp-sdk/issues/23`
> 5. `build(display): wire registry sources into cmake`
> 6. `test(display): unit-test harness for the display registry`
>
> Six commits (one fewer than Camera/Power because no legacy file to remove).
>
> CMakeLists addition (Display has no pre-existing line to replace — purely additive):
> ```cmake
> zephyr_library_sources(
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/display_dispatch.c
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/display/zephyr_stub.c)
> ```
> Place the block near the other registry-pattern blocks (e.g. after the qenc block from slice 4a) — but slice 4a isn't on this branch (we're stacked on adc-pilot, not slice 4a). Place after the ADC registry block, mirroring its style.
>
> Six commit SHAs, verification, report.

---

## Task 4 — Subagent: GPU2D migration (references issue #24)

### Subagent brief (paste verbatim)

> Migrate GPU2D to the registry stub pattern. DO NOT switch branches.
>
> Public surface (5 functions):
> ```
> alp_gpu2d_t *alp_gpu2d_open(void);
> alp_status_t alp_gpu2d_fill_rect(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *dst,
>                                  uint32_t x, uint32_t y, uint32_t w, uint32_t h,
>                                  uint32_t argb_color);
> alp_status_t alp_gpu2d_blit(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *src,
>                             uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst,
>                             uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
> alp_status_t alp_gpu2d_blend(alp_gpu2d_t *handle, const alp_gpu2d_surface_t *src,
>                              uint32_t sx, uint32_t sy, const alp_gpu2d_surface_t *dst,
>                              uint32_t dx, uint32_t dy, uint32_t w, uint32_t h,
>                              alp_gpu2d_blend_mode_t mode);
> void         alp_gpu2d_close(alp_gpu2d_t *handle);
> ```
> Add: `const alp_capabilities_t *alp_gpu2d_capabilities(const alp_gpu2d_t *handle);`
>
> **1-slot pool semantics** like Power — `alp_gpu2d_open` historically opened a single global. Document in commit 3.
>
> **Preserve the surface validation behaviour**: the legacy `gpu2d_zephyr.c` returns `ALP_ERR_INVAL` from `fill_rect`/`blit`/`blend` when `dst`/`src` surfaces are NULL or have zero dimensions, before falling through to `ALP_ERR_NOSUPPORT`. The new stub's ops table SHOULD preserve this: the stub ops bodies validate surface args first, return `ALP_ERR_INVAL` on bad input, otherwise `ALP_ERR_NOT_IMPLEMENTED`. This keeps customer test code that relies on INVAL pre-checks happy.
>
> Stub references issue #24:
> ```
>  * @par Tracking: github.com/alplabai/alp-sdk/issues/24
> ```
>
> Seven commits (Camera-shape, including the legacy file deletion):
> 1. capability decl
> 2. ops.h
> 3. dispatcher
> 4. zephyr_stub.c
> 5. drop legacy `src/zephyr/gpu2d_zephyr.c`
> 6. CMakeLists wiring
> 7. test harness
>
> Test harness specifics: `test_fill_rect_inval_on_null_surface` is an extra case unique to GPU2D — verifies the surface-validation pre-check still fires before `ALP_ERR_NOT_IMPLEMENTED` in the new dispatch path. Use NULL handle + NULL dst surface; expect `ALP_ERR_NOT_READY` (NULL handle short-circuits before surface check). For the stub backend you can't reach via `alp_gpu2d_open` (returns NULL today), so this test exercises the dispatcher's NULL-handle gate, not the stub's INVAL check. Document the constraint in a comment.
>
> Report 7 commit SHAs, verification, deviations.

---

## Task 5 — Orchestrator: ABI regen + PR open

Not a subagent dispatch — orchestrator runs locally.

- [ ] **Step 1: Regenerate ABI snapshot**

```bash
python3 scripts/abi_snapshot.py --version v0.5 --output docs/abi/v0.5-snapshot.json
```

Expected diff: 4 new functions — `alp_camera_capabilities`, `alp_power_capabilities`, `alp_display_capabilities`, `alp_gpu2d_capabilities`.

- [ ] **Step 2: Sanity grep**

```bash
grep -rnE "camera_stub\.c|gpu2d_zephyr\.c|power_zephyr\.c" src/ zephyr/
```

Expected: only documentation comments / Yocto references (none of these subsystems have Yocto files today, so likely zero matches).

- [ ] **Step 3: CI gates**

```bash
python3 scripts/check_stub_issues.py        # exit 0
python3 scripts/check_sw_fallback_tags.py   # exit 0
python3 scripts/check_vendor_ext_tags.py    # exit 0
```

- [ ] **Step 4: Commit ABI**

```bash
git add docs/abi/v0.5-snapshot.json
git commit -m "chore(abi): regenerate v0.5 snapshot for stub-slices"
```

- [ ] **Step 5: Push + open PR**

```bash
git push -u origin feat/backend-registry-stub-slices
gh pr create --base feat/backend-registry-adc-pilot --title "..." --body "..."
```

PR description should include the issue references (#20 #21 #22 #23 #24) and the stacked-PR merge order (after #18, in parallel with #19).

---

## Self-review checklist

- **Spec coverage:** every section in `docs/superpowers/specs/2026-05-22-stub-slices-design.md` maps to a task. §6 test scope → Task 1-4 commit 6/7. §4 stub conventions → Task 1-4 commit 4.
- **No placeholders:** every commit message + every code block is verbatim or has clear "read the header for the exact signature" instructions.
- **Class collisions:** four classes → four DIFFERENT linker sections (`.alp_backends_camera`, `.alp_backends_power`, `.alp_backends_display`, `.alp_backends_gpu2d`). `ALP_BACKEND_DEFINE_CLASS(<class>)` produces unique symbols.
- **CI gate coverage:** `check_stub_issues.py` validates each new `_stub.c` carries the `@par Tracking:` line. Confirmed in all four stub briefs.
