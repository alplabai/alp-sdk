# Simple Peripherals Registry Migration (Slice 4a) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to dispatch ONE subagent per peripheral. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate RTC, WDT, Counter, and QEnc from the legacy `peripheral_<x>.c` pattern to the linker-section backend registry. Four parallelisable peripherals, four sequential subagents, one PR.

**Architecture:** Each peripheral grows a class dispatcher (`src/<class>_dispatch.c`), an internal ops vtable (`src/backends/<class>/<class>_ops.h`), a portable Zephyr backend (`src/backends/<class>/zephyr_drv.c`), and a test-only software backend (`src/backends/<class>/sw_fallback.c`). Counter and QEnc additionally grow a `gd32_bridge.c` backend that takes over the existing V2N supervisor MCU branch. NO vendor extensions ship in this slice — every per-peripheral feature passes the master-spec §3 audit as portable-only.

**Tech Stack:** C17 (ztest on native_sim), Kconfig (4 new `CONFIG_ALP_SDK_<CLASS>_SW_FALLBACK` symbols), Zephyr driver classes (`rtc_*`, `wdt_*`, `counter_*`, `sensor_*`-QDEC), Python 3.10+ (`scripts/abi_snapshot.py`).

**Spec:** `docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md`
**Structural template (canonical):** `docs/superpowers/plans/2026-05-22-adc-registry-pilot.md` and the literal commits `bfc6543..HEAD` of branch `feat/backend-registry-adc-pilot`.
**Branch:** `feat/backend-registry-simple-peripherals` (already created off `feat/backend-registry-adc-pilot` — verify with `git branch --show-current`).
**Foundation PR:** #17 (Slice 0 registry). **Depends-on PR:** #18 (Slice 1 ADC). This slice's PR stacks on #18.

---

## Subagent dispatch convention

Each Task below is one subagent dispatch. The orchestrator:

1. Verifies the working tree is clean and the branch is `feat/backend-registry-simple-peripherals`.
2. Fires the subagent with the brief in the task body verbatim. **Do not paraphrase.** The brief is self-contained because the subagent has no conversation context.
3. After the subagent reports done, runs the verification block at the bottom of each task.
4. Only fires the next subagent if verification passes. On failure, runs `git reset --hard HEAD` and retries with a fresh subagent and a sharper brief.
5. The orchestrator NEVER stages the pre-existing dirty file `include/alp/boards/alp_e1m_evk_routes.h` or any CRLF-only diff on `include/alp/soc_caps.h` — those belong to the user.

**Critical do-nots (carried over from prior session lessons):**
- Subagents must NOT switch branches. The orchestrator confirms branch state up front; the subagent works on whatever is checked out.
- Subagents must NOT touch peripherals other than their assigned class. Cross-class refactors are orchestrator-only.
- Subagents must commit before reporting done. Reported-done with uncommitted changes is treated as a failure.

**Note on `src/zephyr/handles.c`:** that file uses the `DEFINE_POOL(<class>, COUNT_KCONFIG, alp_<class>)` macro to generate the static slot array + acquire/release functions. To strip a peripheral's pool, delete its single `DEFINE_POOL(<class>, ...)` line — that's all. Do NOT modify the macro definition itself.

---

## Pre-flight

- [ ] **Step 1: Verify branch and clean tree**

```bash
git branch --show-current
# expected: feat/backend-registry-simple-peripherals
git status --porcelain
# expected: empty
```

- [ ] **Step 2: Commit the spec + plan**

```bash
git add docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md \
        docs/superpowers/plans/2026-05-22-simple-peripherals-slice4a.md
git commit -m "docs(spec): slice 4a design + plan for RTC/WDT/Counter/QEnc registry migration"
```

---

## Task 1 — Subagent: RTC migration

**Files (all under repo root):**
- Create: `include/alp/rtc.h` — modify to add `alp_rtc_capabilities` decl
- Create: `src/backends/rtc/rtc_ops.h`
- Create: `src/rtc_dispatch.c`
- Create: `src/backends/rtc/zephyr_drv.c`
- Create: `src/backends/rtc/sw_fallback.c`
- Delete: `src/zephyr/peripheral_rtc.c`
- Modify: `src/zephyr/handles.h` (remove `struct alp_rtc` + `alp_z_rtc_pool_*`)
- Modify: `zephyr/Kconfig` (add `CONFIG_ALP_SDK_RTC_SW_FALLBACK`)
- Modify: `zephyr/CMakeLists.txt` (drop `peripheral_rtc.c`, add new sources)
- Create: `tests/unit/rtc_registry/{CMakeLists.txt, prj.conf, testcase.yaml, src/test_rtc_registry.c}`

### Subagent brief (paste verbatim)

> You are migrating the RTC peripheral from `src/zephyr/peripheral_rtc.c` to the backend registry pattern. The structural template is Slice 1 ADC — see `docs/superpowers/plans/2026-05-22-adc-registry-pilot.md` and commits `bfc6543..HEAD` of branch `feat/backend-registry-adc-pilot` (currently checked out → this branch is its child). DO NOT switch branches.
>
> Scope: produce a working RTC dispatcher + Zephyr backend + SW fallback + ztest harness, delete the legacy `peripheral_rtc.c`, and update CMakeLists/Kconfig/handles.h. NO vendor extensions. NO behavioural change beyond the structural lift.
>
> Steps and commits (one commit per step):
>
> **Commit 1 — `feat(rtc): declare alp_rtc_capabilities() public getter`**
>
> Add the declaration to `include/alp/rtc.h` immediately after `alp_rtc_close`. Insert `#include <alp/cap_instance.h>` near other `alp/` includes. Verbatim block:
>
> ```c
> /**
>  * @brief Query the capabilities of an opened RTC handle.
>  *
>  * @param rtc  Handle from @ref alp_rtc_open, or NULL.
>  * @return Pointer valid for the handle's lifetime; NULL if @p rtc is NULL.
>  */
> const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *rtc);
> ```
>
> **Commit 2 — `feat(rtc): private backend ops vtable + handle layout`**
>
> Create `src/backends/rtc/rtc_ops.h`. Pattern matches `src/backends/adc/adc_ops.h`. Layout:
>
> ```c
> #ifndef ALP_BACKENDS_RTC_OPS_H
> #define ALP_BACKENDS_RTC_OPS_H
>
> #include <stdbool.h>
> #include <stdint.h>
> #include <zephyr/device.h>
>
> #include <alp/backend.h>
> #include <alp/cap_instance.h>
> #include <alp/peripheral.h>
> #include <alp/rtc.h>
>
> typedef struct alp_rtc_ops alp_rtc_ops_t;
>
> typedef struct alp_rtc_backend_state {
>     const struct device  *dev;       /* zephyr_drv only; bridges may stash other state */
>     uint32_t              rtc_id;
>     void                 *be_data;
>     const alp_rtc_ops_t  *ops;
> } alp_rtc_backend_state_t;
>
> struct alp_rtc_ops {
>     alp_status_t (*open)(uint32_t rtc_id,
>                          alp_rtc_backend_state_t *state,
>                          alp_capabilities_t *caps_out);
>     alp_status_t (*set_time)(alp_rtc_backend_state_t *state,
>                              const alp_rtc_time_t *time);
>     alp_status_t (*get_time)(alp_rtc_backend_state_t *state,
>                              alp_rtc_time_t *time);
>     void         (*close)(alp_rtc_backend_state_t *state);
> };
>
> struct alp_rtc {
>     alp_rtc_backend_state_t  state;
>     const alp_backend_t     *backend;
>     alp_capabilities_t       cached_caps;
>     bool                     in_use;
> };
>
> #endif /* ALP_BACKENDS_RTC_OPS_H */
> ```
>
> **Commit 3 — `feat(rtc): class dispatcher + handle pool`**
>
> Create `src/rtc_dispatch.c`. Mirrors `src/adc_dispatch.c` shape; trivial differences (no `read_uv` math; ops call `set_time` / `get_time` directly):
>
> ```c
> /* SPDX-License-Identifier: Apache-2.0
>  * RTC class dispatcher.  Routes the public alp_rtc_* surface
>  * through the .alp_backends_rtc registry. */
>
> #include <stdbool.h>
> #include <stddef.h>
> #include <stdint.h>
> #include <string.h>
>
> #include <alp/backend.h>
> #include <alp/cap_instance.h>
> #include <alp/peripheral.h>
> #include <alp/rtc.h>
> #include <alp/soc_caps.h>
>
> #include "backends/rtc/rtc_ops.h"
>
> ALP_BACKEND_DEFINE_CLASS(rtc);
>
> extern void alp_z_set_last_error(alp_status_t s);
> extern void alp_z_clear_last_error(void);
>
> #ifndef CONFIG_ALP_SDK_MAX_RTC_HANDLES
> #define CONFIG_ALP_SDK_MAX_RTC_HANDLES 2
> #endif
>
> static struct alp_rtc _pool[CONFIG_ALP_SDK_MAX_RTC_HANDLES];
>
> static struct alp_rtc *_alloc(void) {
>     for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_RTC_HANDLES; ++i) {
>         if (!_pool[i].in_use) {
>             memset(&_pool[i], 0, sizeof(_pool[i]));
>             _pool[i].in_use = true;
>             return &_pool[i];
>         }
>     }
>     return NULL;
> }
>
> static void _free(struct alp_rtc *h) { h->in_use = false; }
>
> alp_rtc_t *alp_rtc_open(uint32_t rtc_id) {
>     alp_z_clear_last_error();
>     const alp_backend_t *be = alp_backend_select("rtc", ALP_SOC_REF_STR);
>     if (be == NULL) { alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC); return NULL; }
>     const alp_rtc_ops_t *ops = (const alp_rtc_ops_t *)be->ops;
>     if (ops == NULL || ops->open == NULL) {
>         alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED); return NULL;
>     }
>     struct alp_rtc *h = _alloc();
>     if (h == NULL) { alp_z_set_last_error(ALP_ERR_NOMEM); return NULL; }
>     h->backend = be;
>     h->state.ops = ops;
>     alp_capabilities_t caps = { .flags = be->base_caps };
>     if (be->probe != NULL) {
>         uint32_t refined = caps.flags;
>         (void)be->probe(rtc_id, &refined);
>         caps.flags = refined;
>     }
>     alp_status_t rc = ops->open(rtc_id, &h->state, &caps);
>     if (rc != ALP_OK) { _free(h); alp_z_set_last_error(rc); return NULL; }
>     h->cached_caps = caps;
>     return h;
> }
>
> alp_status_t alp_rtc_set_time(alp_rtc_t *h, const alp_rtc_time_t *t) {
>     if (t == NULL) return ALP_ERR_INVAL;
>     if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
>     return h->state.ops->set_time(&h->state, t);
> }
>
> alp_status_t alp_rtc_get_time(alp_rtc_t *h, alp_rtc_time_t *t) {
>     if (t == NULL) return ALP_ERR_INVAL;
>     if (h == NULL || !h->in_use) return ALP_ERR_NOT_READY;
>     return h->state.ops->get_time(&h->state, t);
> }
>
> void alp_rtc_close(alp_rtc_t *h) {
>     if (h == NULL || !h->in_use) return;
>     if (h->state.ops != NULL && h->state.ops->close != NULL) {
>         h->state.ops->close(&h->state);
>     }
>     _free(h);
> }
>
> const alp_capabilities_t *alp_rtc_capabilities(const alp_rtc_t *h) {
>     return (h != NULL) ? &h->cached_caps : NULL;
> }
> ```
>
> **Commit 4 — `feat(rtc): portable Zephyr driver-class backend`**
>
> Create `src/backends/rtc/zephyr_drv.c`. Lifts the existing `peripheral_rtc.c` body verbatim. The DT alias resolution, `errno_to_alp` helper, and `struct rtc_time` conversion all move here unchanged:
>
> ```c
> /* SPDX-License-Identifier: Apache-2.0
>  * Portable Zephyr rtc_* driver-class backend.  Used on every SoC
>  * the SDK ships unless a vendor-specific backend registers a more
>  * specific match. */
>
> #include <errno.h>
> #include <string.h>
>
> #include <zephyr/device.h>
> #include <zephyr/drivers/rtc.h>
> #include <zephyr/sys/util.h>
>
> #include <alp/backend.h>
> #include <alp/cap_instance.h>
> #include <alp/peripheral.h>
> #include <alp/rtc.h>
> #include <alp/soc_caps.h>
>
> #include "rtc_ops.h"
>
> #define ALP_RTC_DEV_OR_NULL(idx) \
>     COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_rtc, idx))), \
>                 (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_rtc, idx)))), (NULL))
>
> static const struct device *const _devs[] = {
>     ALP_RTC_DEV_OR_NULL(0),
>     ALP_RTC_DEV_OR_NULL(1),
> };
>
> static alp_status_t _errno_to_alp(int err) {
>     switch (err) {
>     case 0:        return ALP_OK;
>     case -EINVAL:  return ALP_ERR_INVAL;
>     case -EBUSY:   return ALP_ERR_BUSY;
>     case -ENOTSUP:
>     case -ENOSYS:  return ALP_ERR_NOSUPPORT;
>     default:       return ALP_ERR_IO;
>     }
> }
>
> static alp_status_t z_open(uint32_t rtc_id,
>                            alp_rtc_backend_state_t *st,
>                            alp_capabilities_t *caps_out) {
>     if (rtc_id >= ARRAY_SIZE(_devs)) return ALP_ERR_INVAL;
>     if (rtc_id >= ALP_SOC_RTC_COUNT) return ALP_ERR_OUT_OF_RANGE;
>     const struct device *dev = _devs[rtc_id];
>     if (dev == NULL || !device_is_ready(dev)) return ALP_ERR_NOT_READY;
>     st->dev = dev;
>     st->rtc_id = rtc_id;
>     caps_out->flags = 0u;        /* no per-instance caps in Slice 4a */
>     return ALP_OK;
> }
>
> static alp_status_t z_set_time(alp_rtc_backend_state_t *st,
>                                const alp_rtc_time_t *t) {
>     struct rtc_time zt = {
>         .tm_year = (int)t->year - 1900,
>         .tm_mon  = (int)t->month - 1,
>         .tm_mday = (int)t->day,
>         .tm_wday = (int)t->weekday,
>         .tm_hour = (int)t->hour,
>         .tm_min  = (int)t->minute,
>         .tm_sec  = (int)t->second,
>         .tm_nsec = (int)t->millisecond * 1000000,
>     };
>     return _errno_to_alp(rtc_set_time(st->dev, &zt));
> }
>
> static alp_status_t z_get_time(alp_rtc_backend_state_t *st,
>                                alp_rtc_time_t *t) {
>     struct rtc_time zt;
>     int err = rtc_get_time(st->dev, &zt);
>     if (err != 0) return _errno_to_alp(err);
>     t->year        = (uint16_t)(zt.tm_year + 1900);
>     t->month       = (uint8_t)(zt.tm_mon + 1);
>     t->day         = (uint8_t)zt.tm_mday;
>     t->weekday     = (uint8_t)zt.tm_wday;
>     t->hour        = (uint8_t)zt.tm_hour;
>     t->minute      = (uint8_t)zt.tm_min;
>     t->second      = (uint8_t)zt.tm_sec;
>     t->millisecond = (uint16_t)(zt.tm_nsec / 1000000);
>     return ALP_OK;
> }
>
> static const alp_rtc_ops_t _ops = {
>     .open     = z_open,
>     .set_time = z_set_time,
>     .get_time = z_get_time,
>     .close    = NULL,
> };
>
> ALP_BACKEND_REGISTER(rtc, zephyr_drv, {
>     .silicon_ref = "*",
>     .vendor      = "zephyr",
>     .base_caps   = 0u,
>     .priority    = 100,
>     .ops         = &_ops,
>     .probe       = NULL,
> });
> ```
>
> **Commit 5 — `feat(rtc): SW fallback backend (frozen test clock)`**
>
> Create `src/backends/rtc/sw_fallback.c`. Frozen-clock model: returns `2026-01-01 00:00:00` plus a per-call second increment. The `@par Cost:` and `@par Performance:` tags are MANDATORY (`scripts/check_sw_fallback_tags.py` enforces):
>
> ```c
> /* SPDX-License-Identifier: Apache-2.0
>  *
>  * Software RTC fallback.  Returns a deterministic clock that ticks
>  * one second per get_time call from 2026-01-01 00:00:00.  For
>  * native_sim build / test only.
>  *
>  * @par Cost: ROM ~600 B, RAM 16 B per handle (the alp_rtc_time_t cursor).
>  * @par Performance: O(1) per call; no system clock access.  Frozen-clock
>  *      semantics are intentional -- tests assert exact field values.
>  */
>
> #include <stdint.h>
>
> #include <alp/backend.h>
> #include <alp/cap_instance.h>
> #include <alp/peripheral.h>
> #include <alp/rtc.h>
>
> #include "rtc_ops.h"
>
> static alp_rtc_time_t _cursor = {
>     .year = 2026, .month = 1, .day = 1,
>     .weekday = 4,                         /* Thursday */
>     .hour = 0, .minute = 0, .second = 0, .millisecond = 0,
> };
>
> static alp_status_t sw_open(uint32_t rtc_id,
>                             alp_rtc_backend_state_t *st,
>                             alp_capabilities_t *caps_out) {
>     (void)rtc_id;
>     st->dev = NULL;
>     st->rtc_id = rtc_id;
>     st->be_data = NULL;
>     caps_out->flags = 0u;
>     return ALP_OK;
> }
>
> static alp_status_t sw_set_time(alp_rtc_backend_state_t *st,
>                                 const alp_rtc_time_t *t) {
>     (void)st;
>     _cursor = *t;
>     return ALP_OK;
> }
>
> static alp_status_t sw_get_time(alp_rtc_backend_state_t *st,
>                                 alp_rtc_time_t *t) {
>     (void)st;
>     *t = _cursor;
>     _cursor.second = (uint8_t)((_cursor.second + 1u) % 60u);
>     return ALP_OK;
> }
>
> static const alp_rtc_ops_t _ops = {
>     .open     = sw_open,
>     .set_time = sw_set_time,
>     .get_time = sw_get_time,
>     .close    = NULL,
> };
>
> ALP_BACKEND_REGISTER(rtc, sw_fallback, {
>     .silicon_ref = "*",
>     .vendor      = "sw",
>     .base_caps   = 0u,
>     .priority    = 0,
>     .ops         = &_ops,
>     .probe       = NULL,
> });
> ```
>
> **Commit 6 — `refactor(rtc): drop legacy peripheral_rtc.c + handle pool plumbing`**
>
> Three changes in this commit:
> 1. `git rm src/zephyr/peripheral_rtc.c`
> 2. In `src/zephyr/handles.h`: remove the `struct alp_rtc { ... };` block AND the two pool declarations `alp_z_rtc_pool_acquire` / `alp_z_rtc_pool_release`. Replace the struct block with a comment matching the existing ADC marker: `/* RTC -- struct alp_rtc layout moved to src/backends/rtc/rtc_ops.h (Slice 4a, 2026-05-22) */`.
> 3. In `src/zephyr/handles.c`: remove the line `DEFINE_POOL(rtc, CONFIG_ALP_SDK_MAX_RTC_HANDLES, alp_rtc)`. The pool functions are macro-generated from this single line; deleting it strips both the static array and the acquire/release functions in one go.
>
> Verify with: `grep -nE "alp_z_rtc_pool|peripheral_rtc|DEFINE_POOL\\(rtc" src/ zephyr/` — must return zero matches.
>
> **Commit 7 — `build(rtc): wire registry sources into cmake + add CONFIG_ALP_SDK_RTC_SW_FALLBACK`**
>
> Two file changes in this commit:
> 1. `zephyr/CMakeLists.txt`: replace the line that adds `src/zephyr/peripheral_rtc.c` with a block matching the ADC pattern (look for `src/adc_dispatch.c` in the same file as the template):
>
> ```cmake
> zephyr_library_sources(
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/rtc_dispatch.c
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rtc/zephyr_drv.c)
>
> if(CONFIG_ALP_SDK_RTC_SW_FALLBACK)
>     zephyr_library_sources(
>         ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/rtc/sw_fallback.c)
> endif()
> ```
>
> 2. `zephyr/Kconfig`: add (near the existing `CONFIG_ALP_SDK_ADC_SW_FALLBACK` block):
>
> ```kconfig
> config ALP_SDK_RTC_SW_FALLBACK
>     bool "Software RTC fallback (deterministic frozen clock)"
>     depends on ALP_SDK
>     default y if BOARD_NATIVE_SIM || ARCH_POSIX
>     default n
>     help
>       Compiles a software RTC backend that registers as a wildcard
>       at priority 0.  Picked only when no hardware backend matches.
>       Frozen-clock semantics -- ticks 1 s per get_time call from
>       2026-01-01 00:00:00.  Tests use this; production firmware
>       should keep it off.
> ```
>
> **Commit 8 — `test(rtc): unit-test harness for the RTC registry`**
>
> Create the four files under `tests/unit/rtc_registry/`. Pattern matches `tests/unit/adc_registry/` exactly:
>
> `CMakeLists.txt`:
> ```cmake
> # SPDX-License-Identifier: Apache-2.0
> cmake_minimum_required(VERSION 3.20.0)
> find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
> project(test_rtc_registry)
> target_sources(app PRIVATE src/test_rtc_registry.c)
> ```
>
> `prj.conf`:
> ```
> CONFIG_ZTEST=y
> CONFIG_ALP_SDK=y
> CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
> CONFIG_ALP_SDK_RTC_SW_FALLBACK=y
> ```
>
> `testcase.yaml`:
> ```yaml
> tests:
>   alp.unit.rtc_registry:
>     platform_allow:
>       - native_sim
>       - native_sim/native/64
>     integration_platforms:
>       - native_sim
>       - native_sim/native/64
>     tags:
>       - alp
>       - rtc
>       - backend
>       - unit
> ```
>
> `src/test_rtc_registry.c` — same fake-handle pattern as `tests/unit/adc_registry/src/test_adc_registry.c`. Six minimum cases:
>
> 1. `test_zephyr_drv_picked_over_sw_on_alif_e7` — `alp_backend_select("rtc", "alif:ensemble:e7")` returns the `zephyr_drv` backend (vendor "zephyr", priority 100).
> 2. `test_sw_fallback_picked_for_unknown_silicon` — `alp_backend_select("rtc", "fictional:soc:zz")` returns the `sw_fallback` backend (vendor "sw", priority 0).
> 3. `test_select_returns_null_for_null_class` — `alp_backend_select(NULL, "alif:ensemble:e7")` is NULL.
> 4. `test_select_returns_null_for_null_silicon_ref` — `alp_backend_select("rtc", NULL)` is NULL (Slice 0 regression test).
> 5. `test_open_returns_inval_on_null_id_oob` — call `alp_rtc_open(99)` against the fake fictional ref; assert NULL.
> 6. `test_capabilities_returns_null_for_null_handle` — `alp_rtc_capabilities(NULL)` is NULL.
> 7. `test_backend_count_for_rtc` — `alp_backend_count("rtc")` returns 2 (zephyr_drv + sw_fallback).
>
> Include the ops header via relative path matching the ADC harness: `#include "../../../../src/backends/rtc/rtc_ops.h"` so the test can construct fake handles directly.
>
> **Final reporting checklist** before declaring done:
> - `git log --oneline feat/backend-registry-adc-pilot..HEAD` shows 8 commits.
> - `git status --porcelain` is empty.
> - `grep -rnE "alp_z_rtc_pool|peripheral_rtc\\.c" src/ zephyr/` returns zero matches.
> - `python3 scripts/check_sw_fallback_tags.py` exits 0.
> - `python3 scripts/check_vendor_ext_tags.py` exits 0 (no new vendor extensions, gate is a no-op).
>
> Report any deviations from the brief or any unexpected files you had to touch. Do NOT touch `include/alp/boards/alp_e1m_evk_routes.h` or `include/alp/soc_caps.h` (CRLF noise) — they're not yours.

### Verification (orchestrator)

```bash
git log --oneline feat/backend-registry-adc-pilot..HEAD
# expect: 8 commits prefixed feat(rtc)/refactor(rtc)/build(rtc)/test(rtc)
ls src/backends/rtc src/rtc_dispatch.c tests/unit/rtc_registry/
test -f src/zephyr/peripheral_rtc.c && echo FAIL || echo OK
grep -rnE "alp_z_rtc_pool|peripheral_rtc\\.c" src/ zephyr/ || echo OK
python3 scripts/check_sw_fallback_tags.py
```

---

## Task 2 — Subagent: WDT migration

**Files:**
- Modify: `include/alp/wdt.h` (add `alp_wdt_capabilities` decl)
- Create: `src/backends/wdt/wdt_ops.h`, `src/wdt_dispatch.c`, `src/backends/wdt/zephyr_drv.c`, `src/backends/wdt/sw_fallback.c`
- Delete: `src/zephyr/peripheral_wdt.c`
- Modify: `src/zephyr/handles.h`, `src/zephyr/handles.c`, `zephyr/Kconfig`, `zephyr/CMakeLists.txt`
- Create: `tests/unit/wdt_registry/{CMakeLists.txt, prj.conf, testcase.yaml, src/test_wdt_registry.c}`

### Subagent brief (paste verbatim)

> You are migrating the WDT peripheral. The structural template is RTC (Task 1, just committed — see `git log --oneline -10` for the eight rtc-prefixed commits and copy the shape exactly). Match the public API in `include/alp/wdt.h`: `alp_wdt_open(uint32_t, const alp_wdt_config_t *)`, `alp_wdt_feed`, `alp_wdt_disable`, `alp_wdt_close`. DO NOT switch branches.
>
> Same 8-commit cadence: capabilities decl → ops.h → dispatcher → zephyr_drv → sw_fallback → drop legacy + handles cleanup → CMakeLists/Kconfig wiring → test harness.
>
> **Per-step deltas from RTC:**
>
> `wdt_ops.h` carries:
> ```c
> typedef struct alp_wdt_ops {
>     alp_status_t (*open)(uint32_t wdt_id, const alp_wdt_config_t *cfg,
>                          alp_wdt_backend_state_t *state,
>                          alp_capabilities_t *caps_out);
>     alp_status_t (*feed)(alp_wdt_backend_state_t *state);
>     alp_status_t (*disable)(alp_wdt_backend_state_t *state);
>     void         (*close)(alp_wdt_backend_state_t *state);
> } alp_wdt_ops_t;
> ```
>
> `alp_wdt_backend_state_t` carries `const struct device *dev`, `uint32_t wdt_id`, `int channel_id`, `alp_wdt_config_t cfg`, `void *be_data`, `const alp_wdt_ops_t *ops`.
>
> `struct alp_wdt` (moved from handles.h) carries `state`, `backend`, `cached_caps`, `in_use`. Layout mirrors `struct alp_rtc` from Task 1.
>
> `zephyr_drv.c` `open` lifts the body of `peripheral_wdt.c::alp_wdt_open` verbatim, including the `wdt_install_timeout` + `wdt_setup` calls and the `alp_wdt_action_t` → `WDT_FLAG_*` translation. The error paths return `ALP_ERR_*` instead of NULL because the dispatcher handles NULL framing.
>
> `sw_fallback.c` is a stub: `feed` and `disable` return `ALP_OK`, `open` initialises the state and returns `ALP_OK`. Still requires the `@par Cost:` and `@par Performance:` tags.
>
> `tests/unit/wdt_registry/`: same six baseline cases as RTC, swap `alp_rtc_*` for `alp_wdt_*` and `"rtc"` class for `"wdt"`. Test 5 becomes `test_open_returns_null_on_null_cfg` (asserts `alp_wdt_open(0, NULL)` returns NULL).
>
> Same final reporting checklist as RTC. Eight commits.

### Verification (orchestrator)

```bash
git log --oneline feat/backend-registry-adc-pilot..HEAD | head -20
# expect: 16 total commits (8 rtc + 8 wdt)
ls src/backends/wdt src/wdt_dispatch.c tests/unit/wdt_registry/
test -f src/zephyr/peripheral_wdt.c && echo FAIL || echo OK
grep -rnE "alp_z_wdt_pool|peripheral_wdt\\.c" src/ zephyr/ || echo OK
python3 scripts/check_sw_fallback_tags.py
```

---

## Task 3 — Subagent: Counter migration (with V2N bridge backend)

**Files:**
- Modify: `include/alp/counter.h` (add `alp_counter_capabilities` decl — counter half only; QEnc is Task 4)
- Create: `src/backends/counter/counter_ops.h`, `src/counter_dispatch.c`, `src/backends/counter/zephyr_drv.c`, `src/backends/counter/sw_fallback.c`, `src/backends/counter/gd32_bridge.c`
- Delete: `src/zephyr/peripheral_counter.c`
- Modify: `src/zephyr/handles.h`, `src/zephyr/handles.c`, `zephyr/Kconfig`, `zephyr/CMakeLists.txt`
- Create: `tests/unit/counter_registry/{CMakeLists.txt, prj.conf, testcase.yaml, src/test_counter_registry.c}`

### Subagent brief (paste verbatim)

> You are migrating the Counter peripheral. Two non-trivial deltas from RTC/WDT:
> 1. Counter has 7 public functions (`open`, `start`, `stop`, `get_value`, `us_to_ticks`, `set_alarm`, `cancel_alarm`, `close`) and an alarm callback type. Ops vtable mirrors all 7.
> 2. Counter has a V2N bridge backend. The current `peripheral_counter.c` uses `#if CONFIG_ALP_SDK_V2N_SUPERVISOR` to switch — split that into a separate `gd32_bridge.c` backend registering for `silicon_ref = "renesas:rzv2n:n44"`, and the `zephyr_drv.c` backend keeps the non-bridge code path.
>
> DO NOT touch QEnc — that's Task 4. `<alp/counter.h>` covers both surfaces but they're independent classes. Add `alp_counter_capabilities` decl ONLY in this task; `alp_qenc_capabilities` decl lands in Task 4.
>
> Nine commits this task (one extra for the bridge backend):
>
> **Commits 1–5** match RTC/WDT pattern (capabilities decl, ops.h, dispatcher, zephyr_drv, sw_fallback).
>
> **Commit 6 — `feat(counter): V2N GD32 bridge backend`**
>
> Create `src/backends/counter/gd32_bridge.c`. Lifts the `#if ALP_COUNTER_HAS_BRIDGE_PATH` branches of the current `peripheral_counter.c` into a dedicated backend. Register:
>
> ```c
> ALP_BACKEND_REGISTER(counter, gd32_bridge, {
>     .silicon_ref = "renesas:rzv2n:n44",
>     .vendor      = "renesas",
>     .base_caps   = 0u,           /* set_alarm returns NOSUPPORT on bridge */
>     .priority    = 100,
>     .ops         = &_ops,
>     .probe       = NULL,
> });
> ```
>
> `open` rejects `counter_id >= 1` (same as today's behaviour). `set_alarm` and `us_to_ticks` return `ALP_ERR_NOSUPPORT` (matches today). `get_value` calls `gd32g553_counter_read` via the supervisor singleton — copy the existing acquire/release pattern from `peripheral_counter.c` verbatim. `start`/`stop` return `ALP_OK` (counter is free-running on the GD32 side).
>
> Include `<chips/gd32g553/v2n_supervisor.h>` via the same relative path the legacy code uses (`#include "v2n_supervisor.h"` works because `src/zephyr/` is already on the include path for the SDK).
>
> **Commit 7 — `refactor(counter): drop legacy peripheral_counter.c + handle pool plumbing`**
>
> Same shape as RTC commit 6. Note: `struct alp_counter` carries `alarm_cb` + `alarm_user` — those fields move INTO `counter_ops.h`'s `struct alp_counter` layout. The `zephyr_drv.c` backend's `set_alarm` reaches into `h->alarm_cb` directly. The `counter_alarm_trampoline` static helper from the legacy file moves into `zephyr_drv.c`.
>
> **Commit 8 — `build(counter): wire registry sources + add CONFIG_ALP_SDK_COUNTER_SW_FALLBACK`**
>
> CMakeLists block must gate the bridge backend on `CONFIG_ALP_SDK_V2N_SUPERVISOR` (only the V2N build wants it linked):
>
> ```cmake
> zephyr_library_sources(
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/counter_dispatch.c
>     ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/counter/zephyr_drv.c)
>
> if(CONFIG_ALP_SDK_V2N_SUPERVISOR)
>     zephyr_library_sources(
>         ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/counter/gd32_bridge.c)
> endif()
>
> if(CONFIG_ALP_SDK_COUNTER_SW_FALLBACK)
>     zephyr_library_sources(
>         ${ZEPHYR_CURRENT_MODULE_DIR}/src/backends/counter/sw_fallback.c)
> endif()
> ```
>
> Kconfig: add `CONFIG_ALP_SDK_COUNTER_SW_FALLBACK` mirroring the RTC entry.
>
> **Commit 9 — `test(counter): unit-test harness for counter registry`**
>
> Same six baseline cases as RTC, PLUS two bridge-selector cases:
> - `test_bridge_picked_over_zephyr_on_v2n` — `alp_backend_select("counter", "renesas:rzv2n:n44")` returns the `gd32_bridge` backend. NOTE: this test only runs when `CONFIG_ALP_SDK_V2N_SUPERVISOR=y`. Skip the case with `Z_TEST_SKIP_IFNDEF(CONFIG_ALP_SDK_V2N_SUPERVISOR)` if needed; the simpler alternative is to NOT compile the bridge backend on this test build and instead test selection logic with a manually-crafted backend pointer.
> - `test_set_alarm_capability_clear_on_bridge_open` — open against `renesas:rzv2n:n44`, assert capabilities flags do NOT include `ALP_INSTANCE_CAP_HW_ALARM`.
>
> Simpler path: leave the bridge backend OUT of the test prj.conf (don't set `CONFIG_ALP_SDK_V2N_SUPERVISOR`), and write the bridge test as a TODO comment in the test file. Document this in the commit message.
>
> Eight or nine commits total depending on the bridge-test path. Same final reporting checklist as RTC/WDT.

### Verification (orchestrator)

```bash
git log --oneline feat/backend-registry-adc-pilot..HEAD | head -30
ls src/backends/counter src/counter_dispatch.c tests/unit/counter_registry/
test -f src/zephyr/peripheral_counter.c && echo FAIL || echo OK
grep -rnE "alp_z_counter_pool|peripheral_counter\\.c" src/ zephyr/ || echo OK
python3 scripts/check_sw_fallback_tags.py
```

---

## Task 4 — Subagent: QEnc migration (with V2N bridge backend)

**Files:**
- Modify: `include/alp/counter.h` (add `alp_qenc_capabilities` decl)
- Create: `src/backends/qenc/qenc_ops.h`, `src/qenc_dispatch.c`, `src/backends/qenc/zephyr_drv.c`, `src/backends/qenc/sw_fallback.c`, `src/backends/qenc/gd32_bridge.c`
- Delete: `src/zephyr/peripheral_qenc.c`
- Modify: `src/zephyr/handles.h`, `src/zephyr/handles.c`, `zephyr/Kconfig`, `zephyr/CMakeLists.txt`
- Create: `tests/unit/qenc_registry/{CMakeLists.txt, prj.conf, testcase.yaml, src/test_qenc_registry.c}`

### Subagent brief (paste verbatim)

> You are migrating the QEnc peripheral. Structural template: Counter (Task 3). DO NOT switch branches.
>
> Public surface is 4 functions: `alp_qenc_open(const alp_qenc_config_t *)`, `alp_qenc_get_position`, `alp_qenc_reset_position`, `alp_qenc_close`. Ops vtable has 4 entries.
>
> `qenc_ops.h`:
> ```c
> typedef struct alp_qenc_ops {
>     alp_status_t (*open)(const alp_qenc_config_t *cfg,
>                          alp_qenc_backend_state_t *state,
>                          alp_capabilities_t *caps_out);
>     alp_status_t (*get_position)(alp_qenc_backend_state_t *state, int32_t *pos_out);
>     alp_status_t (*reset_position)(alp_qenc_backend_state_t *state);
>     void         (*close)(alp_qenc_backend_state_t *state);
> } alp_qenc_ops_t;
> ```
>
> `alp_qenc_backend_state_t` carries `const struct device *dev`, `uint32_t encoder_id`, `int32_t last_position`, `void *be_data`, `const alp_qenc_ops_t *ops`.
>
> `struct alp_qenc` (moves out of handles.h) is `state`, `backend`, `cached_caps`, `in_use`.
>
> `zephyr_drv.c` lifts the Zephyr sensor-API branch of `peripheral_qenc.c::alp_qenc_get_position` — the `sensor_sample_fetch` + `sensor_channel_get(... SENSOR_CHAN_ROTATION ...)` calls plus the `enc->last_position += v.val1` accumulation.
>
> `gd32_bridge.c` registers for `silicon_ref = "renesas:rzv2n:n44"` and lifts the bridge branch (calls `gd32g553_qenc_read` / `gd32g553_qenc_reset` via the supervisor singleton).
>
> `sw_fallback.c` is a deterministic counter: `get_position` returns `state->last_position` and increments it by 7 per call.
>
> Same 8-commit cadence as Counter (the bridge backend gets its own commit). Test harness mirrors the Counter harness's six baseline cases.
>
> CMakeLists wiring follows Counter's pattern: bridge gated on `CONFIG_ALP_SDK_V2N_SUPERVISOR`, sw_fallback gated on `CONFIG_ALP_SDK_QENC_SW_FALLBACK`. Kconfig: add `CONFIG_ALP_SDK_QENC_SW_FALLBACK`.
>
> Same final reporting checklist. Report on commit count and verification grep results.

### Verification (orchestrator)

```bash
git log --oneline feat/backend-registry-adc-pilot..HEAD | head -40
ls src/backends/qenc src/qenc_dispatch.c tests/unit/qenc_registry/
test -f src/zephyr/peripheral_qenc.c && echo FAIL || echo OK
grep -rnE "alp_z_qenc_pool|peripheral_qenc\\.c" src/ zephyr/ || echo OK
python3 scripts/check_sw_fallback_tags.py
```

---

## Task 5 — Orchestrator: ABI snapshot regen + final integration

NOT a subagent dispatch. Orchestrator runs locally.

- [ ] **Step 1: Regenerate ABI snapshot**

```bash
python3 scripts/abi_snapshot.py --version v0.5 --output docs/abi/v0.5-snapshot.json
```

Expected diff: 4 new functions added — `alp_rtc_capabilities`, `alp_wdt_capabilities`, `alp_counter_capabilities`, `alp_qenc_capabilities`. No other surface changes.

- [ ] **Step 2: Sanity-grep no stale handles references**

```bash
grep -rnE "alp_z_(rtc|wdt|counter|qenc)_pool|peripheral_(rtc|wdt|counter|qenc)\\.c" src/ zephyr/
```

Expected: zero matches.

- [ ] **Step 3: Confirm CI gates still pass**

```bash
python3 scripts/check_sw_fallback_tags.py    # exit 0
python3 scripts/check_vendor_ext_tags.py     # exit 0 (no new vendor exts)
python3 scripts/check_stub_issues.py         # exit 0 (no stubs)
```

- [ ] **Step 4: Commit the ABI refresh**

```bash
git add docs/abi/v0.5-snapshot.json
git commit -m "chore(abi): regenerate v0.5 snapshot for simple-peripherals slice"
```

- [ ] **Step 5: Push + open PR**

```bash
git push -u origin feat/backend-registry-simple-peripherals
gh pr create --base feat/backend-registry-adc-pilot \
             --title "feat(periph): RTC/WDT/Counter/QEnc registry migration (Slice 4a)" \
             --body "$(cat <<'EOF'
## Summary

Migrates RTC, WDT, Counter, and QEnc from the legacy `peripheral_<x>.c` `#if`-ladder pattern to the linker-section backend registry. Follows the Slice 1 ADC structural template (PR #18) for four mechanical peripherals.

Per-peripheral surface:
- Class dispatcher at `src/<class>_dispatch.c`
- Internal ops vtable at `src/backends/<class>/<class>_ops.h`
- Portable Zephyr driver-class backend at `src/backends/<class>/zephyr_drv.c` (priority 100, wildcard)
- Software fallback at `src/backends/<class>/sw_fallback.c` (priority 0, wildcard, with `@par Cost:` / `@par Performance:` tags)
- (Counter + QEnc only) GD32 supervisor bridge at `src/backends/<class>/gd32_bridge.c` (priority 100, `renesas:rzv2n:n44`)
- One unit-test harness per class at `tests/unit/<class>_registry/`

Net deltas:
- 4 new public functions: `alp_<class>_capabilities()` per class (declarations in existing headers; no new public headers).
- 4 new Kconfig symbols: `CONFIG_ALP_SDK_{RTC,WDT,COUNTER,QENC}_SW_FALLBACK`.
- 4 legacy `src/zephyr/peripheral_<class>.c` files removed (~558 lines).
- ABI snapshot refreshed.

**No vendor extensions in this slice.** Every per-peripheral feature passed the master-spec §3 audit as portable-only. See `docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md` §5 for the audit table.

## Stacked PR order

Merge after:
1. #14 capability-api → main
2. #15 board-yaml-diagnostics → main
3. #17 backend-registry-foundation → main
4. #18 backend-registry-adc-pilot → main
5. **this PR** → main

Until #18 lands, the base branch is `feat/backend-registry-adc-pilot` so the diff stays reviewable.

## Test plan

- [ ] All four `tests/unit/<class>_registry/` harnesses pass under twister on native_sim.
- [ ] CI gates (`check_sw_fallback_tags`, `check_vendor_ext_tags`, `check_stub_issues`) stay green.
- [ ] ABI snapshot diff matches the expected 4 new function additions.
- [ ] V2N Zephyr build still resolves Counter + QEnc via the GD32 bridge backend (not the wildcard Zephyr backend).
- [ ] Alif Zephyr build still resolves Counter + QEnc via the wildcard Zephyr backend.

## Design references

- Spec: `docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md`
- Plan: `docs/superpowers/plans/2026-05-22-simple-peripherals-slice4a.md`
- Structural template (Slice 1): `docs/superpowers/specs/2026-05-22-adc-registry-pilot-design.md` and PR #18
EOF
)"
```

---

## Self-review checklist

After all four subagents complete and Task 5 lands:

- **Spec coverage:** every section of the spec maps to a task.
  - §1 file layout → Tasks 1–4 create every file
  - §2 backend matrix → Tasks 1–4 register all backends with the listed (silicon_ref, priority, vendor) triples
  - §3 ops vtables → Tasks 1–4 commit 2 in each task
  - §4 portable layer → Tasks 1–4 commit 1 in each task (capabilities decl) + commit 3 (dispatcher)
  - §5 vendor-ext audit → zero new files under `include/alp/ext/`
  - §6 test scope → Tasks 1–4 commit 8 (or 9 for counter)
  - §7 subagent dispatch shape → exactly four subagents, sequentially
- **Placeholder scan:** no TBD / TODO / "fill in later" in any of the code snippets above.
- **Type consistency:** `alp_<class>_ops_t`, `alp_<class>_backend_state_t`, `struct alp_<class>` are referenced with consistent names across all four tasks.
- **Cross-class collisions:** four classes are registered into four DIFFERENT linker sections (`.alp_backends_rtc`, `.alp_backends_wdt`, `.alp_backends_counter`, `.alp_backends_qenc`). `ALP_BACKEND_DEFINE_CLASS(<class>)` produces unique symbols. Confirmed by inspecting the macro definition in `include/alp/backend.h`.
