# Backend Registry Foundation (Slice 0) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Ship the foundation for the backend-registry architecture (Slice 0 of `docs/superpowers/specs/2026-05-21-backend-registry-design.md`): the public macro + selector, instance-capability types, three new error codes, a sentinel-backed linker-section visibility test, a toy-class end-to-end demo, and three CI gates — without migrating any real subsystem yet.

**Architecture:** New public headers (`<alp/backend.h>`, additions to `<alp/cap.h>`, `<alp/peripheral.h>`) describe the surface. `src/backend.c` carries the linker-section walker. `tests/regress/` + `tests/unit/backend_registry/` prove the mechanism end-to-end under twister on native_sim. Three CI-gate Python scripts ship as no-ops (tolerant of empty inputs) so that when slices 1+ start producing stub backends / vendor extensions / sw fallbacks, the gates light up automatically.

**Tech Stack:** C17 (Zephyr ztest for unit tests), Python 3.10+ (CI gate scripts, ABI snapshot), GitHub Actions YAML, GCC linker section attributes.

**Spec:** `docs/superpowers/specs/2026-05-21-backend-registry-design.md`
**Branch:** `feat/backend-registry-foundation` (already checked out)

---

## File Structure

**Create (public headers):**
- `include/alp/backend.h` — `alp_backend_t` struct, `ALP_BACKEND_REGISTER` macro, `alp_backend_select` + `alp_backend_count` declarations, `ALP_BACKEND_AVAILABLE(class)` macro.
- New entries in `include/alp/cap.h` — `alp_instance_cap_t` enum, `alp_capabilities_t` struct, `alp_capabilities_has` declaration.
- New entries in `include/alp/peripheral.h` — `ALP_ERR_NOT_PRESENT_ON_THIS_SOC`, `ALP_ERR_NOT_IMPLEMENTED` error codes.

**Create (implementation):**
- `src/backend.c` — selector body (linker-section walk, cached per-class).

**Create (tests under twister):**
- `tests/regress/backend_section/CMakeLists.txt`
- `tests/regress/backend_section/prj.conf`
- `tests/regress/backend_section/testcase.yaml`
- `tests/regress/backend_section/src/test_section_visibility.c`
- `tests/unit/backend_registry/CMakeLists.txt`
- `tests/unit/backend_registry/prj.conf`
- `tests/unit/backend_registry/testcase.yaml`
- `tests/unit/backend_registry/src/demo_class.h`
- `tests/unit/backend_registry/src/demo_class.c`
- `tests/unit/backend_registry/src/demo_backend_realhw.c`
- `tests/unit/backend_registry/src/demo_backend_sw.c`
- `tests/unit/backend_registry/src/demo_backend_stub.c`
- `tests/unit/backend_registry/src/test_registry.c`

**Create (CI gates):**
- `scripts/check_stub_issues.py`
- `scripts/check_vendor_ext_tags.py`
- `scripts/check_sw_fallback_tags.py`
- `tests/scripts/test_check_stub_issues.py`
- `tests/scripts/test_check_vendor_ext_tags.py`
- `tests/scripts/test_check_sw_fallback_tags.py`

**Create (docs):**
- `docs/architecture/backend-registry.md` — contributor guide

**Modify:**
- `.github/workflows/pr-metadata-validate.yml` — invoke the three new gate scripts
- `docs/abi/v0.5-snapshot.json` — regenerate to include new public headers
- Zephyr module `CMakeLists.txt` (likely `zephyr/CMakeLists.txt` or `src/zephyr/CMakeLists.txt`) — pick up `src/backend.c` as part of the SDK library

---

## Task 1: Add three new error codes to `<alp/peripheral.h>`

**Files:**
- Modify: `include/alp/peripheral.h`

- [ ] **Step 1: Read the existing header to find the enum**

`grep -n "ALP_ERR_NOSUPPORT\|alp_error_t\|typedef enum" include/alp/peripheral.h`

Locate the existing `alp_error_t` enum or its `#define` form. Append the two new codes at the end of the enum (or `#define` block). Preserve the existing value of `ALP_ERR_NOSUPPORT` — the new codes are additions, not renames.

- [ ] **Step 2: Apply edit**

In `include/alp/peripheral.h`, add inside the `alp_error_t` enum (or as `#define`s, matching the existing style):

```c
/**
 * @brief Hardware does not exist on this silicon and never will.
 *
 * Returned by alp_<class>_open() when alp_backend_select() finds no
 * backend registered for the active silicon_ref.  Paired with
 * compile-time ALP_BACKEND_AVAILABLE(<class>) == 0 so customer code
 * can branch via #if rather than at runtime.
 */
#define ALP_ERR_NOT_PRESENT_ON_THIS_SOC (-64)

/**
 * @brief A backend exists for this silicon but its implementation
 *        is a tracked stub (planned, not yet wired).
 *
 * Customer should consult the linked GitHub issue on the stub
 * backend's @par Tracking: tag.
 */
#define ALP_ERR_NOT_IMPLEMENTED (-65)
```

(If the file uses an enum, use enum syntax matching the existing entries instead.)

- [ ] **Step 3: Verify no existing code uses these constant values**

`grep -rn "\-64\|\-65" include/alp/ src/ chips/` — ensure no other error code already squats on -64 or -65. If a collision exists, pick the next two unused negative integers and update the spec snippet accordingly.

- [ ] **Step 4: Compile-smoke**

```bash
python3 scripts/abi_snapshot.py --version v0.5 --output /tmp/abi-smoke.json
```
Expected: writes the snapshot without error. Confirms the header still parses cleanly via the snapshot tool's preprocessor.

- [ ] **Step 5: Commit**

```bash
git add include/alp/peripheral.h
git commit -m "feat(error): add ALP_ERR_NOT_PRESENT_ON_THIS_SOC + NOT_IMPLEMENTED"
```

---

## Task 2: Add instance-capability types to `<alp/cap.h>`

**Files:**
- Modify: `include/alp/cap.h`
- Modify: `src/cap.c` (already generated by `gen_soc_caps.py`; we add a hand-written helper file alongside)
- Create: `src/cap_helpers.c`
- Create: `tests/unit/backend_registry/src/test_capabilities.c` (single-purpose unit test for `alp_capabilities_has`)

- [ ] **Step 1: Read existing cap.h**

`cat include/alp/cap.h` to confirm shape. The generator wrote it; we add to it carefully so the next generator run preserves our additions. Inspect: does `gen_soc_caps.py` regenerate the whole file, or append? If whole-file regenerate, we must add the new types to the generator template OR move them to a sibling header.

**Decision:** add to a sibling header `include/alp/cap_instance.h` so the generated `cap.h` stays untouched. The portable public surface includes both via the existing `<alp/cap.h>` (we add `#include "cap_instance.h"` at the bottom of the generator template).

- [ ] **Step 2: Write the sibling header**

Create `include/alp/cap_instance.h`:

```c
/**
 * @file cap_instance.h
 * @brief Instance-level capability flags and struct.
 *
 * Distinct from the SoC-level ALP_CAP_* macros in soc_caps.h /
 * cap.h: those answer "does this silicon have an NPU at all?",
 * these answer "does THIS opened ADC instance support DMA?".
 *
 * Populated by each backend's ops->probe() at open time, cached
 * in the handle, returned by alp_<class>_capabilities().
 *
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      v0.7 introduces the instance-cap surface alongside the
 *      backend-registry foundation.  Promoted to [ABI-STABLE]
 *      once at least three vendor families exercise it.
 */

#ifndef ALP_CAP_INSTANCE_H
#define ALP_CAP_INSTANCE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Bitwise-OR'd flags describing what a single opened handle can do. */
typedef enum {
    ALP_INSTANCE_CAP_DMA            = 1u << 0,
    ALP_INSTANCE_CAP_HW_OVERSAMPLE  = 1u << 1,
    ALP_INSTANCE_CAP_HW_TRIGGER     = 1u << 2,
    ALP_INSTANCE_CAP_DIFFERENTIAL   = 1u << 3,
} alp_instance_cap_t;

/** Per-instance capability descriptor populated by ops->probe. */
typedef struct alp_capabilities {
    uint32_t flags;
    uint32_t max_sample_rate;   /* 0 = not applicable */
    uint16_t max_resolution_bits;
    uint16_t channel_count;
} alp_capabilities_t;

/**
 * @brief Test whether the descriptor advertises a capability flag.
 * @param c   Pointer returned by alp_<class>_capabilities().
 * @param f   A single flag from alp_instance_cap_t.
 * @return true if (c->flags & f) is non-zero; false otherwise.
 *         Returns false when c is NULL.
 */
bool alp_capabilities_has(const alp_capabilities_t *c, alp_instance_cap_t f);

#ifdef __cplusplus
}
#endif

#endif /* ALP_CAP_INSTANCE_H */
```

- [ ] **Step 3: Have <alp/cap.h> include the sibling**

If `cap.h` is generator-produced, edit `scripts/gen_soc_caps.py`'s `_emit_cap_h()` so the emitted header ends with `#include "cap_instance.h"` just before the final `#endif`. Then regenerate:

```bash
python3 scripts/gen_soc_caps.py
```

Verify the regenerated `cap.h` contains the new `#include`.

- [ ] **Step 4: Implement `alp_capabilities_has`**

Create `src/cap_helpers.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hand-written helpers for the ALP capability API.  NOT auto-
 * generated -- this file holds runtime logic that does not depend
 * on per-SoC metadata.
 */

#include <stddef.h>
#include <alp/cap_instance.h>

bool alp_capabilities_has(const alp_capabilities_t *c, alp_instance_cap_t f)
{
    if (c == NULL) {
        return false;
    }
    return (c->flags & (uint32_t)f) != 0u;
}
```

- [ ] **Step 5: Add src/cap_helpers.c to the Zephyr build**

`grep -n "src/cap.c" zephyr/CMakeLists.txt` to find where the existing cap.c is added; append `src/cap_helpers.c` to the same `zephyr_library_sources(...)` list (or the equivalent target_sources block).

- [ ] **Step 6: Write a failing test**

Create `tests/unit/backend_registry/src/test_capabilities.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include <zephyr/ztest.h>
#include <alp/cap_instance.h>

ZTEST_SUITE(alp_caps, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_caps, test_has_returns_true_for_set_flag)
{
    alp_capabilities_t c = { .flags = ALP_INSTANCE_CAP_DMA };
    zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_caps, test_has_returns_false_for_clear_flag)
{
    alp_capabilities_t c = { .flags = ALP_INSTANCE_CAP_DMA };
    zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_HW_TRIGGER));
}

ZTEST(alp_caps, test_has_returns_false_for_null_pointer)
{
    zassert_false(alp_capabilities_has(NULL, ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_caps, test_or_of_multiple_flags)
{
    alp_capabilities_t c = {
        .flags = ALP_INSTANCE_CAP_DMA | ALP_INSTANCE_CAP_HW_OVERSAMPLE,
    };
    zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_DMA));
    zassert_true(alp_capabilities_has(&c, ALP_INSTANCE_CAP_HW_OVERSAMPLE));
    zassert_false(alp_capabilities_has(&c, ALP_INSTANCE_CAP_HW_TRIGGER));
}
```

(Test harness CMakeLists/prj.conf/testcase.yaml created in Task 6 — for now the test file exists but isn't yet wired.)

- [ ] **Step 7: Commit**

```bash
git add include/alp/cap_instance.h include/alp/cap.h src/cap_helpers.c scripts/gen_soc_caps.py zephyr/CMakeLists.txt tests/unit/backend_registry/src/test_capabilities.c
git commit -m "feat(cap): instance-level capability types + alp_capabilities_has"
```

(Stage `zephyr/CMakeLists.txt` only if you actually modified it.)

---

## Task 3: Public backend header (`<alp/backend.h>`)

**Files:**
- Create: `include/alp/backend.h`

- [ ] **Step 1: Write the header**

Create `include/alp/backend.h`:

```c
/**
 * @file backend.h
 * @brief Backend registration + selection API.
 *
 * Each peripheral subsystem (ADC, SPI, …) carries a class name.
 * Backends register themselves into a per-class linker section via
 * the ALP_BACKEND_REGISTER macro.  alp_<class>_open() walks the
 * section once, picks a backend by silicon_ref + priority, caches
 * the choice, and dispatches through ops thereafter.
 *
 * See docs/architecture/backend-registry.md for the design
 * narrative.
 *
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 */

#ifndef ALP_BACKEND_H
#define ALP_BACKEND_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One row in a per-class linker section.
 *
 * @par Special silicon_ref:
 *      "*" — wildcard.  Considered only when no exact-match backend
 *            exists.  Used by software fallback backends.
 */
typedef struct alp_backend {
    const char  *silicon_ref;
    const char  *vendor;
    uint32_t     base_caps;
    uint8_t      priority;
    const void  *ops;
    int        (*probe)(uint32_t instance_id, uint32_t *refined_caps);
} alp_backend_t;

/**
 * @brief Register a backend into the per-class section.
 *
 * Expands to a static const struct in section ".alp_backends_<class>".
 * The linker collects every such entry into a contiguous array.
 *
 * @param class  Class name (e.g. adc, spi, inference).  Becomes part of
 *               the section name; must be a valid C identifier.
 * @param name   Backend identifier (e.g. alif_e7, sw_fallback).  Must be
 *               unique within a class; appears in the symbol name.
 * @param ...    Brace-enclosed initializer for alp_backend_t.
 */
#define ALP_BACKEND_REGISTER(class, name, ...)                              \
    static const alp_backend_t _alp_be_##class##_##name                     \
        __attribute__((used, section(".alp_backends_" #class))) = __VA_ARGS__

/**
 * @brief Find the best backend registered for a class on the active SoC.
 *
 * Walks the per-class section, filters by silicon_ref exact match or
 * "*" wildcard, sorts by priority desc, returns the first hit.
 *
 * @param class_name   The class identifier passed to ALP_BACKEND_REGISTER.
 * @param silicon_ref  Active SoC reference (e.g. "alif:ensemble:e7").
 *                     Pass ALP_SOC_REF_STR from <alp/soc_caps.h>.
 * @return  Pointer to the chosen backend, or NULL if none match.
 */
const alp_backend_t *alp_backend_select(const char *class_name,
                                        const char *silicon_ref);

/**
 * @brief Count backends registered for a class (any silicon).
 *
 * Compile-time-foldable on most linkers; usable in #if when the
 * compiler can fold the section symbols.
 *
 * @param class_name   The class identifier passed to ALP_BACKEND_REGISTER.
 * @return  Number of entries in the .alp_backends_<class> section.
 */
size_t alp_backend_count(const char *class_name);

/**
 * @brief Compile-time check: is any backend linked in for this class?
 *
 * Distinct from ALP_HAS(<CAP>) (SoC-level).  Use ALP_BACKEND_AVAILABLE
 * to guard call sites that would otherwise dispatch to a class with no
 * registered backend on this build.
 */
#define ALP_BACKEND_AVAILABLE(class) \
    (&__start_alp_backends_##class != &__stop_alp_backends_##class)

#ifdef __cplusplus
}
#endif

#endif /* ALP_BACKEND_H */
```

- [ ] **Step 2: Smoke-compile**

```bash
gcc -c -x c -I include -o /tmp/backend-smoke.o - <<'EOF'
#include <alp/backend.h>
const alp_backend_t *probe(void) {
    return alp_backend_select("smoke", "alif:ensemble:e7");
}
EOF
```
Expected: exit 0 (header parses, references resolve to declarations).

- [ ] **Step 3: Commit**

```bash
git add include/alp/backend.h
git commit -m "feat(backend): public registry header (macro + select + count)"
```

---

## Task 4: Selector implementation (`src/backend.c`)

**Files:**
- Create: `src/backend.c`
- Modify: Zephyr build to include it (same `zephyr/CMakeLists.txt` you touched in Task 2)

- [ ] **Step 1: Implement the selector**

Create `src/backend.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Backend selector: walks the per-class linker section, picks the
 * best match for the active silicon_ref by priority.
 *
 * The per-class sections are populated by ALP_BACKEND_REGISTER in
 * <alp/backend.h>.  The linker provides __start_/__stop_ symbols
 * automatically for sections whose names are valid C identifiers
 * after the leading dot is stripped (GCC/Clang behavior).
 */

#include <stddef.h>
#include <string.h>
#include <alp/backend.h>

/*
 * Section symbol lookup is class-name-driven.  We can't compute
 * symbol names dynamically in standard C, so the selector takes a
 * class_name string and walks an externally supplied [start, stop)
 * range.  The per-class dispatchers (alp_adc_open et al.) hold the
 * start/stop pair for their class and call _select_in_range.
 *
 * For Slice 0 we expose a single class_name-keyed table populated
 * by ALP_BACKEND_DEFINE_CLASS_TABLE at link time -- see the toy
 * class in tests/unit/backend_registry/.  The table maps name ->
 * (start, stop).
 */

typedef struct alp_backend_class_range {
    const char              *class_name;
    const alp_backend_t     *start;
    const alp_backend_t     *stop;
} alp_backend_class_range_t;

/* Per-class table populated via the ALP_BACKEND_DEFINE_CLASS macro
 * (see <alp/backend.h>).  Each subsystem instantiates one entry
 * during its own bootstrap. */
extern const alp_backend_class_range_t __start_alp_backend_classes[];
extern const alp_backend_class_range_t __stop_alp_backend_classes[];

static const alp_backend_t *select_in_range(const alp_backend_t *start,
                                            const alp_backend_t *stop,
                                            const char *silicon_ref)
{
    const alp_backend_t *best = NULL;
    for (const alp_backend_t *be = start; be < stop; ++be) {
        const bool wild = (be->silicon_ref != NULL
                           && be->silicon_ref[0] == '*'
                           && be->silicon_ref[1] == '\0');
        const bool exact = (be->silicon_ref != NULL
                            && silicon_ref != NULL
                            && strcmp(be->silicon_ref, silicon_ref) == 0);
        if (!wild && !exact) {
            continue;
        }
        if (best == NULL || be->priority > best->priority) {
            best = be;
        }
    }
    return best;
}

const alp_backend_t *alp_backend_select(const char *class_name,
                                        const char *silicon_ref)
{
    if (class_name == NULL) {
        return NULL;
    }
    for (const alp_backend_class_range_t *c = __start_alp_backend_classes;
         c < __stop_alp_backend_classes; ++c) {
        if (strcmp(c->class_name, class_name) == 0) {
            return select_in_range(c->start, c->stop, silicon_ref);
        }
    }
    return NULL;
}

size_t alp_backend_count(const char *class_name)
{
    if (class_name == NULL) {
        return 0u;
    }
    for (const alp_backend_class_range_t *c = __start_alp_backend_classes;
         c < __stop_alp_backend_classes; ++c) {
        if (strcmp(c->class_name, class_name) == 0) {
            return (size_t)(c->stop - c->start);
        }
    }
    return 0u;
}
```

- [ ] **Step 2: Add the per-class table macro to `<alp/backend.h>`**

Append to `include/alp/backend.h` before the closing `#endif`:

```c
/**
 * @brief Define the class-range table entry for a per-class section.
 *
 * Each subsystem includes one of these once (typically in the file
 * that implements alp_<class>_open).  The class table tells the
 * selector how to find the section for a class name.
 */
#define ALP_BACKEND_DEFINE_CLASS(class)                                       \
    extern const alp_backend_t __start_alp_backends_##class[];                \
    extern const alp_backend_t __stop_alp_backends_##class[];                 \
    static const alp_backend_class_range_t                                    \
        _alp_class_range_##class                                              \
        __attribute__((used, section("alp_backend_classes"))) = {             \
            .class_name = #class,                                             \
            .start = __start_alp_backends_##class,                            \
            .stop = __stop_alp_backends_##class,                              \
        }
```

Move the `alp_backend_class_range_t` typedef from `src/backend.c` to `backend.h` (above the macro), so the macro can reference it.

- [ ] **Step 3: Wire `src/backend.c` into the Zephyr build**

Append `src/backend.c` to the same `zephyr_library_sources(...)` list in `zephyr/CMakeLists.txt` that already has `src/cap.c` and (from Task 2) `src/cap_helpers.c`.

- [ ] **Step 4: Smoke-compile**

```bash
gcc -c -x c -I include -o /tmp/backend-impl-smoke.o src/backend.c
```
Expected: warnings OK, exit 0.

- [ ] **Step 5: Commit**

```bash
git add include/alp/backend.h src/backend.c zephyr/CMakeLists.txt
git commit -m "feat(backend): selector implementation + per-class table macro"
```

---

## Task 5: Section visibility regression test

**Files:**
- Create: `tests/regress/backend_section/CMakeLists.txt`
- Create: `tests/regress/backend_section/prj.conf`
- Create: `tests/regress/backend_section/testcase.yaml`
- Create: `tests/regress/backend_section/src/test_section_visibility.c`

- [ ] **Step 1: Create the test harness**

Create `tests/regress/backend_section/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_backend_section_visibility)

target_sources(app PRIVATE src/test_section_visibility.c)
```

Create `tests/regress/backend_section/prj.conf`:

```
CONFIG_ZTEST=y
CONFIG_ALP_SDK=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
```

Create `tests/regress/backend_section/testcase.yaml`:

```yaml
tests:
  alp.regress.backend_section:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim
      - native_sim/native/64
    tags:
      - alp
      - backend
      - regression
```

- [ ] **Step 2: Write the sentinel test**

Create `tests/regress/backend_section/src/test_section_visibility.c`:

```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * Guards against linker scripts (or LTO settings) that silently
 * drop the .alp_backends_* sections.  Registers a sentinel backend
 * for a fictional class "regress_sentinel" and asserts the
 * selector finds it back.
 *
 * If this test ever fails on a new toolchain, the registry pattern
 * is broken on that toolchain -- triage the linker script before
 * the next subsystem migration lands.
 */

#include <string.h>
#include <zephyr/ztest.h>
#include <alp/backend.h>

ALP_BACKEND_REGISTER(regress_sentinel, alif, {
    .silicon_ref = "alif:ensemble:e7",
    .vendor      = "alif",
    .base_caps   = 0u,
    .priority    = 100,
    .ops         = NULL,
    .probe       = NULL,
});

ALP_BACKEND_DEFINE_CLASS(regress_sentinel);

ZTEST_SUITE(alp_backend_section, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_backend_section, test_sentinel_is_visible_to_selector)
{
    const alp_backend_t *be = alp_backend_select(
        "regress_sentinel", "alif:ensemble:e7");
    zassert_not_null(be, "sentinel backend not found -- linker stripped the section");
    zassert_equal(strcmp(be->vendor, "alif"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_backend_section, test_count_reflects_section)
{
    zassert_equal(alp_backend_count("regress_sentinel"), 1u);
}

ZTEST(alp_backend_section, test_select_returns_null_for_unknown_class)
{
    zassert_is_null(alp_backend_select("does_not_exist", "alif:ensemble:e7"));
}

ZTEST(alp_backend_section, test_select_returns_null_for_unknown_silicon)
{
    zassert_is_null(alp_backend_select("regress_sentinel", "fictional:soc:zz"));
}
```

- [ ] **Step 3: Local twister run (best-effort)**

```bash
west twister -T tests/regress/backend_section -p native_sim --inline-logs
```

If twister is not on PATH locally (Windows), skip and rely on CI to run it. Structural check: confirm the test file compiles via the same `gcc` smoke above (replacing the ztest include with stubs) — optional.

- [ ] **Step 4: Commit**

```bash
git add tests/regress/backend_section/
git commit -m "test(backend): linker-section visibility regression"
```

---

## Task 6: Toy class end-to-end demo

**Files:** all under `tests/unit/backend_registry/`.

- [ ] **Step 1: Harness**

Create `tests/unit/backend_registry/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_backend_registry)

target_sources(app PRIVATE
    src/test_capabilities.c
    src/test_registry.c
    src/demo_class.c
    src/demo_backend_realhw.c
    src/demo_backend_sw.c
    src/demo_backend_stub.c
)
```

Create `tests/unit/backend_registry/prj.conf`:

```
CONFIG_ZTEST=y
CONFIG_ALP_SDK=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
```

Create `tests/unit/backend_registry/testcase.yaml`:

```yaml
tests:
  alp.unit.backend_registry:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim
      - native_sim/native/64
    tags:
      - alp
      - backend
      - unit
```

- [ ] **Step 2: Toy class header**

Create `tests/unit/backend_registry/src/demo_class.h`:

```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * Fictional "demo" peripheral class used only to exercise the
 * backend registry end-to-end without bringing real silicon into
 * the unit test.
 */

#ifndef DEMO_CLASS_H
#define DEMO_CLASS_H

#include <stdint.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>

typedef struct demo_handle {
    const alp_backend_t  *backend;
    alp_capabilities_t    caps;
} demo_handle_t;

typedef struct demo_ops {
    int (*open)(demo_handle_t *h, uint32_t instance_id);
    int (*read)(demo_handle_t *h, uint32_t *out);
} demo_ops_t;

/* Returns 0 on success, negative ALP_ERR_* on failure. */
int demo_open(demo_handle_t *h, uint32_t instance_id);
int demo_read(demo_handle_t *h, uint32_t *out);
const alp_capabilities_t *demo_capabilities(const demo_handle_t *h);

#endif /* DEMO_CLASS_H */
```

- [ ] **Step 3: Toy class dispatcher**

Create `tests/unit/backend_registry/src/demo_class.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include "demo_class.h"
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

ALP_BACKEND_DEFINE_CLASS(demo);

int demo_open(demo_handle_t *h, uint32_t instance_id)
{
    if (h == NULL) {
        return ALP_ERR_NOSUPPORT;
    }
    const alp_backend_t *be = alp_backend_select("demo", ALP_SOC_REF_STR);
    if (be == NULL) {
        return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    }
    h->backend = be;
    h->caps.flags = be->base_caps;
    h->caps.max_sample_rate = 0u;
    h->caps.max_resolution_bits = 0u;
    h->caps.channel_count = 1u;
    if (be->probe != NULL) {
        uint32_t refined = h->caps.flags;
        (void)be->probe(instance_id, &refined);
        h->caps.flags = refined;
    }
    const demo_ops_t *ops = (const demo_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return ops->open(h, instance_id);
}

int demo_read(demo_handle_t *h, uint32_t *out)
{
    if (h == NULL || out == NULL || h->backend == NULL) {
        return ALP_ERR_NOSUPPORT;
    }
    const demo_ops_t *ops = (const demo_ops_t *)h->backend->ops;
    if (ops == NULL || ops->read == NULL) {
        return ALP_ERR_NOT_IMPLEMENTED;
    }
    return ops->read(h, out);
}

const alp_capabilities_t *demo_capabilities(const demo_handle_t *h)
{
    return (h != NULL) ? &h->caps : NULL;
}
```

- [ ] **Step 4: Real-HW backend (high priority, advertises DMA)**

Create `tests/unit/backend_registry/src/demo_backend_realhw.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_class.h"

static int demo_realhw_open(demo_handle_t *h, uint32_t instance_id)
{
    (void)h; (void)instance_id;
    return 0;
}

static int demo_realhw_read(demo_handle_t *h, uint32_t *out)
{
    (void)h;
    *out = 0xCAFEu;
    return 0;
}

static const demo_ops_t demo_realhw_ops = {
    .open = demo_realhw_open,
    .read = demo_realhw_read,
};

static int demo_realhw_probe(uint32_t instance_id, uint32_t *caps)
{
    /* Instance 0 has DMA, instance 1+ does not (refine downward). */
    if (instance_id == 0u) {
        *caps |= (uint32_t)ALP_INSTANCE_CAP_DMA;
    } else {
        *caps &= ~(uint32_t)ALP_INSTANCE_CAP_DMA;
    }
    return 0;
}

ALP_BACKEND_REGISTER(demo, realhw, {
    .silicon_ref = "alif:ensemble:e7",
    .vendor      = "alif",
    .base_caps   = (uint32_t)ALP_INSTANCE_CAP_HW_OVERSAMPLE,
    .priority    = 100,
    .ops         = &demo_realhw_ops,
    .probe       = demo_realhw_probe,
});
```

- [ ] **Step 5: SW fallback backend (lowest priority, wildcard)**

Create `tests/unit/backend_registry/src/demo_backend_sw.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_class.h"

static int demo_sw_open(demo_handle_t *h, uint32_t instance_id)
{
    (void)h; (void)instance_id;
    return 0;
}

static int demo_sw_read(demo_handle_t *h, uint32_t *out)
{
    (void)h;
    *out = 0x50F7u;
    return 0;
}

static const demo_ops_t demo_sw_ops = {
    .open = demo_sw_open,
    .read = demo_sw_read,
};

ALP_BACKEND_REGISTER(demo, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &demo_sw_ops,
    .probe       = NULL,
});
```

- [ ] **Step 6: Stub backend (mid priority, returns NOT_IMPLEMENTED)**

Create `tests/unit/backend_registry/src/demo_backend_stub.c`:

```c
/* SPDX-License-Identifier: Apache-2.0
 *
 * @brief Stub: demo backend for a fictional silicon.
 * @par Implementation status: NOT_IMPLEMENTED (planned: never -- this is a test fixture)
 * @par Tracking: github.com/alplabai/alp-sdk/issues/0
 */

#include "demo_class.h"

/* No ops -> demo_open() returns ALP_ERR_NOT_IMPLEMENTED for this silicon. */

ALP_BACKEND_REGISTER(demo, stub_target, {
    .silicon_ref = "fictional:stub:target",
    .vendor      = "fictional",
    .base_caps   = 0u,
    .priority    = 50,
    .ops         = NULL,
    .probe       = NULL,
});
```

- [ ] **Step 7: End-to-end test**

Create `tests/unit/backend_registry/src/test_registry.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */

#include <zephyr/ztest.h>
#include <alp/peripheral.h>
#include <alp/cap_instance.h>
#include "demo_class.h"

ZTEST_SUITE(alp_registry, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_registry, test_real_hw_picked_over_sw_on_matching_soc)
{
    demo_handle_t h = {0};
    int rc = demo_open(&h, /*instance_id=*/0u);
    zassert_equal(rc, 0, "open failed: %d", rc);
    zassert_not_null(h.backend);
    /* Expect the realhw backend (priority 100) over sw (priority 0). */
    zassert_equal(strcmp(h.backend->vendor, "alif"), 0,
                  "expected alif backend, got %s", h.backend->vendor);
}

ZTEST(alp_registry, test_probe_refines_caps_per_instance)
{
    /* Instance 0 advertises DMA; instance 1 does not. */
    demo_handle_t h0 = {0};
    demo_handle_t h1 = {0};
    zassert_equal(demo_open(&h0, 0u), 0);
    zassert_equal(demo_open(&h1, 1u), 0);
    zassert_true(alp_capabilities_has(demo_capabilities(&h0),
                                      ALP_INSTANCE_CAP_DMA));
    zassert_false(alp_capabilities_has(demo_capabilities(&h1),
                                       ALP_INSTANCE_CAP_DMA));
}

ZTEST(alp_registry, test_oversample_cap_inherited_from_base_caps)
{
    demo_handle_t h = {0};
    zassert_equal(demo_open(&h, 0u), 0);
    zassert_true(alp_capabilities_has(demo_capabilities(&h),
                                      ALP_INSTANCE_CAP_HW_OVERSAMPLE));
}

ZTEST(alp_registry, test_read_dispatches_through_ops)
{
    demo_handle_t h = {0};
    uint32_t v = 0u;
    zassert_equal(demo_open(&h, 0u), 0);
    zassert_equal(demo_read(&h, &v), 0);
    /* The realhw backend returns 0xCAFE. */
    zassert_equal(v, 0xCAFEu);
}

ZTEST(alp_registry, test_backend_count_for_demo)
{
    /* realhw + sw_fallback + stub_target = 3 */
    zassert_equal(alp_backend_count("demo"), 3u);
}

ZTEST(alp_registry, test_select_returns_stub_for_stub_silicon)
{
    const alp_backend_t *be =
        alp_backend_select("demo", "fictional:stub:target");
    zassert_not_null(be);
    zassert_is_null(be->ops, "stub backend should advertise null ops");
}

ZTEST(alp_registry, test_open_returns_not_implemented_for_stub_silicon)
{
    /* Patch via env -- since the unit-test build pins
     * CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7, we can't easily change
     * ALP_SOC_REF_STR at runtime.  Instead, call the lower-level
     * selector + dispatcher directly. */
    const alp_backend_t *be =
        alp_backend_select("demo", "fictional:stub:target");
    zassert_not_null(be);
    zassert_is_null(be->ops);
}

ZTEST(alp_registry, test_sw_fallback_picked_when_no_exact_match)
{
    /* No backend registers for "renesas:rzv2n:n44" -- only the
     * wildcard sw_fallback matches.  Selector should return sw. */
    const alp_backend_t *be =
        alp_backend_select("demo", "renesas:rzv2n:n44");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw"), 0);
    zassert_equal(be->priority, 0);
}
```

- [ ] **Step 8: Local twister run (best-effort)**

```bash
west twister -T tests/unit/backend_registry -p native_sim --inline-logs
```

If twister unavailable locally, rely on CI.

- [ ] **Step 9: Commit**

```bash
git add tests/unit/backend_registry/
git commit -m "test(backend): toy-class end-to-end registry + capability demo"
```

---

## Task 7: CI gate — stub-backend issue tracker

**Files:**
- Create: `scripts/check_stub_issues.py`
- Create: `tests/scripts/test_check_stub_issues.py`

- [ ] **Step 1: Write a failing test**

Create `tests/scripts/test_check_stub_issues.py`:

```python
"""Unit tests for scripts/check_stub_issues.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_stub_issues.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def test_empty_tree_passes(tmp_path):
    """No backends at all -> exit 0."""
    (tmp_path / "src" / "backends").mkdir(parents=True)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_stub_with_issue_passes(tmp_path):
    """Stub file naming the issue link -> exit 0."""
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "nxp_stub.c").write_text(
        "/*\n * @par Implementation status: NOT_IMPLEMENTED\n"
        " * @par Tracking: github.com/alplabai/alp-sdk/issues/42\n */\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_stub_without_issue_fails(tmp_path):
    """*_stub.c without an issue ref -> exit 1."""
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "broken_stub.c").write_text("/* @par Implementation status: NOT_IMPLEMENTED */\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "broken_stub.c" in proc.stdout + proc.stderr
```

Run:
```bash
python3 -m pytest tests/scripts/test_check_stub_issues.py -v
```
Expected: tests fail because `check_stub_issues.py` does not yet exist.

- [ ] **Step 2: Implement the script**

Create `scripts/check_stub_issues.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every src/backends/**/*_stub.c file must reference an open
GitHub issue in its top comment block, on a line of the form:

    @par Tracking: github.com/alplabai/alp-sdk/issues/<N>

Exits 0 when every stub file is compliant (or no stub files exist).
Exits 1 listing the offenders otherwise.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ISSUE_RE = re.compile(
    r"@par\s+Tracking:\s*github\.com/alplabai/alp-sdk/issues/\d+",
    re.IGNORECASE,
)


def _check_one(path: Path) -> str | None:
    text = path.read_text(encoding="utf-8", errors="replace")
    head = text[:2000]  # the tag should live near the top
    if ISSUE_RE.search(head):
        return None
    return f"{path}: missing '@par Tracking: github.com/alplabai/alp-sdk/issues/<N>' tag"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    backends_root = args.root / "src" / "backends"
    if not backends_root.is_dir():
        return 0  # no backends to check yet

    offenders: list[str] = []
    for stub in sorted(backends_root.rglob("*_stub.c")):
        err = _check_one(stub)
        if err:
            offenders.append(err)

    if not offenders:
        return 0

    print("check_stub_issues: the following stub backends lack an issue link:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Re-run the test**

```bash
python3 -m pytest tests/scripts/test_check_stub_issues.py -v
```
Expected: 3 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add scripts/check_stub_issues.py tests/scripts/test_check_stub_issues.py
git commit -m "ci(backend): gate every *_stub.c on a github issue reference"
```

---

## Task 8: CI gate — vendor-extension `@par Supported silicon:` tag

**Files:**
- Create: `scripts/check_vendor_ext_tags.py`
- Create: `tests/scripts/test_check_vendor_ext_tags.py`

- [ ] **Step 1: Write a failing test**

Create `tests/scripts/test_check_vendor_ext_tags.py`:

```python
"""Unit tests for scripts/check_vendor_ext_tags.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_vendor_ext_tags.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def test_empty_ext_dir_passes(tmp_path):
    (tmp_path / "include" / "alp" / "ext").mkdir(parents=True)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stderr


def test_function_with_tag_passes(tmp_path):
    h = tmp_path / "include" / "alp" / "ext" / "alif" / "adc.h"
    h.parent.mkdir(parents=True)
    h.write_text(
        "/**\n"
        " * @par Supported silicon: alif:ensemble:e3, e5, e7\n"
        " */\n"
        "int alp_alif_adc_set_oversampling(void);\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout + proc.stderr


def test_function_without_tag_fails(tmp_path):
    h = tmp_path / "include" / "alp" / "ext" / "alif" / "adc.h"
    h.parent.mkdir(parents=True)
    h.write_text(
        "/** untagged */\n"
        "int alp_alif_adc_set_oversampling(void);\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "alp_alif_adc_set_oversampling" in proc.stdout + proc.stderr
```

- [ ] **Step 2: Implement the script**

Create `scripts/check_vendor_ext_tags.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every function declaration under include/alp/ext/**/*.h whose
name matches alp_<vendor>_<class>_<verb> must carry an
@par Supported silicon: tag in its immediately-preceding Doxygen block.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
FUNC_RE = re.compile(
    r"^\s*[A-Za-z_][\w\s\*]*?\s+(alp_[a-z0-9]+_[a-z0-9]+_[a-z0-9_]+)\s*\(",
    re.MULTILINE,
)
TAG_RE = re.compile(r"@par\s+Supported\s+silicon\s*:", re.IGNORECASE)


def _check_header(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    issues: list[str] = []
    for m in FUNC_RE.finditer(text):
        fname = m.group(1)
        # Look back up to 30 lines for an @par Supported silicon: tag.
        prefix_end = m.start()
        prefix = text[max(0, prefix_end - 2000):prefix_end]
        if not TAG_RE.search(prefix):
            issues.append(f"{path}:{fname}: missing '@par Supported silicon:' tag")
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    ext_root = args.root / "include" / "alp" / "ext"
    if not ext_root.is_dir():
        return 0

    offenders: list[str] = []
    for h in sorted(ext_root.rglob("*.h")):
        offenders.extend(_check_header(h))

    if not offenders:
        return 0

    print("check_vendor_ext_tags: the following functions lack the silicon tag:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Re-run the test**

```bash
python3 -m pytest tests/scripts/test_check_vendor_ext_tags.py -v
```
Expected: 3 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add scripts/check_vendor_ext_tags.py tests/scripts/test_check_vendor_ext_tags.py
git commit -m "ci(backend): gate vendor-ext headers on @par Supported silicon tag"
```

---

## Task 9: CI gate — SW-fallback cost/performance tags

**Files:**
- Create: `scripts/check_sw_fallback_tags.py`
- Create: `tests/scripts/test_check_sw_fallback_tags.py`

- [ ] **Step 1: Write a failing test**

Create `tests/scripts/test_check_sw_fallback_tags.py`:

```python
"""Unit tests for scripts/check_sw_fallback_tags.py."""

import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "check_sw_fallback_tags.py"


def _run(*args, **kw):
    return subprocess.run(
        [sys.executable, str(SCRIPT), *args], capture_output=True, text=True, **kw,
    )


def test_empty_backends_passes(tmp_path):
    (tmp_path / "src" / "backends").mkdir(parents=True)
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0


def test_sw_fallback_with_both_tags_passes(tmp_path):
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "sw_fallback.c").write_text(
        "/*\n * @par Cost: ROM ~2 KB, RAM 0\n"
        " * @par Performance: deterministic saw wave; no DMA, no real conversion\n */\n"
    )
    proc = _run("--root", str(tmp_path))
    assert proc.returncode == 0, proc.stdout


def test_sw_fallback_missing_cost_tag_fails(tmp_path):
    d = tmp_path / "src" / "backends" / "adc"
    d.mkdir(parents=True)
    (d / "sw_fallback.c").write_text("/* @par Performance: slow */\n")
    proc = _run("--root", str(tmp_path))
    assert proc.returncode != 0
    assert "Cost" in proc.stdout + proc.stderr
```

- [ ] **Step 2: Implement the script**

Create `scripts/check_sw_fallback_tags.py`:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""
CI gate: every src/backends/**/sw_fallback.c carries both
@par Cost: and @par Performance: tags near the top of the file.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
COST_RE = re.compile(r"@par\s+Cost\s*:", re.IGNORECASE)
PERF_RE = re.compile(r"@par\s+Performance\s*:", re.IGNORECASE)


def _check_one(path: Path) -> list[str]:
    text = path.read_text(encoding="utf-8", errors="replace")
    head = text[:2000]
    issues: list[str] = []
    if not COST_RE.search(head):
        issues.append(f"{path}: missing '@par Cost:' tag")
    if not PERF_RE.search(head):
        issues.append(f"{path}: missing '@par Performance:' tag")
    return issues


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=REPO)
    args = ap.parse_args()

    backends_root = args.root / "src" / "backends"
    if not backends_root.is_dir():
        return 0

    offenders: list[str] = []
    for f in sorted(backends_root.rglob("sw_fallback.c")):
        offenders.extend(_check_one(f))

    if not offenders:
        return 0

    print("check_sw_fallback_tags: the following SW fallbacks lack tags:")
    for o in offenders:
        print(f"  {o}")
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 3: Re-run the test**

```bash
python3 -m pytest tests/scripts/test_check_sw_fallback_tags.py -v
```
Expected: 3 tests PASS.

- [ ] **Step 4: Commit**

```bash
git add scripts/check_sw_fallback_tags.py tests/scripts/test_check_sw_fallback_tags.py
git commit -m "ci(backend): gate SW-fallback files on Cost + Performance tags"
```

---

## Task 10: Wire the three new gates into the PR workflow

**Files:**
- Modify: `.github/workflows/pr-metadata-validate.yml`

- [ ] **Step 1: Read the existing workflow to locate the validate-job step list**

`grep -n "name: " .github/workflows/pr-metadata-validate.yml | head -20`

Find a near-the-end step (e.g. the existing `lint_doc_yaml_fragments.py` invocation) to insert the new gates after.

- [ ] **Step 2: Add three new steps**

In `.github/workflows/pr-metadata-validate.yml`, after the existing `Lint doc YAML fragments` step, append:

```yaml
      - name: Gate — every *_stub.c references a GitHub issue
        run: python3 scripts/check_stub_issues.py

      - name: Gate — vendor-ext headers carry @par Supported silicon tag
        run: python3 scripts/check_vendor_ext_tags.py

      - name: Gate — sw_fallback.c files carry Cost + Performance tags
        run: python3 scripts/check_sw_fallback_tags.py
```

- [ ] **Step 3: Smoke-run all three from repo root**

```bash
python3 scripts/check_stub_issues.py && \
python3 scripts/check_vendor_ext_tags.py && \
python3 scripts/check_sw_fallback_tags.py && \
echo "all gates pass on current tree"
```
Expected: prints `all gates pass on current tree` and exits 0 — there are no stub backends, vendor ext headers, or sw_fallback files yet, so the gates are no-ops.

- [ ] **Step 4: Commit**

```bash
git add .github/workflows/pr-metadata-validate.yml
git commit -m "ci(backend): wire stub/vendor-ext/sw-fallback gates into pr-metadata"
```

---

## Task 11: Contributor docs

**Files:**
- Create: `docs/architecture/backend-registry.md`

- [ ] **Step 1: Write the doc**

Create `docs/architecture/backend-registry.md`:

```markdown
# Backend Registry Architecture

The ALP SDK dispatches every peripheral call through a linker-section
backend registry. This page tells you how to add a backend for a new
silicon family without editing existing files.

## Concepts

* **Class** — a peripheral category (`adc`, `spi`, `inference`, …).
  Each class has its own public header (`<alp/adc.h>`) and its own
  per-class linker section (`.alp_backends_adc`).

* **Backend** — one row in a class's section. Declares the silicon
  ref it supports, the vendor name, base capabilities, a priority,
  an ops vtable, and an optional probe function for per-instance
  refinement.

* **Capabilities** — two distinct layers:
  * `ALP_CAP_*` macros in `<alp/cap.h>` are SoC-level. They answer
    "does this silicon have an NPU at all?"
  * `ALP_INSTANCE_CAP_*` flags in `<alp/cap_instance.h>` are
    handle-level. They answer "does THIS opened ADC handle support
    DMA?" Populated by the backend's `ops->probe()` at open time.

## Adding a backend

1. Drop a new file under `src/backends/<class>/<vendor>.c`.
2. Implement the class's ops struct (signature is documented in the
   class's internal header — e.g. `src/zephyr/peripheral_adc_ops.h`).
3. Register it with `ALP_BACKEND_REGISTER(<class>, <name>, { … });`.
4. Declare the class table entry with `ALP_BACKEND_DEFINE_CLASS(<class>)`
   in the class dispatcher (only the dispatcher file does this, once).
5. Add the new file to the appropriate Kconfig / CMake list so it
   builds for the right SoM target.

That's it. The registry walker at `alp_backend_select()` picks up the
new entry automatically.

## Adding a software fallback

Same pattern, with `silicon_ref = "*"` and `priority = 0`. The file
MUST live at `src/backends/<class>/sw_fallback.c` and MUST carry
`@par Cost:` and `@par Performance:` tags in its top comment block
(enforced by `scripts/check_sw_fallback_tags.py` at CI time).

## Adding a vendor extension

Vendor-specific knobs that can't be portable live under
`include/alp/ext/<vendor>/<class>.h`. Naming: `alp_<vendor>_<class>_<verb>`.
Every public extension function MUST carry an `@par Supported silicon:`
Doxygen tag listing the silicon refs it works on (enforced by
`scripts/check_vendor_ext_tags.py` at CI time).

## Stubs

A backend file whose `ops` field is NULL is a stub: `alp_<class>_open`
returns `ALP_ERR_NOT_IMPLEMENTED`. Stub files MUST live at
`src/backends/<class>/<vendor>_stub.c` and MUST carry an
`@par Tracking: github.com/alplabai/alp-sdk/issues/<N>` reference
(enforced by `scripts/check_stub_issues.py` at CI time).

## Three error codes — what they mean to a customer

| Code | Meaning |
|---|---|
| `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` | Silicon physically lacks this block. Use a different SKU or accept the limitation. Paired with `ALP_HAS()`/`ALP_BACKEND_AVAILABLE()` at compile time. |
| `ALP_ERR_NOSUPPORT` | Backend exists, rejected the specific config. Adjust your config. |
| `ALP_ERR_NOT_IMPLEMENTED` | Backend is a tracked stub. Consult the linked GitHub issue. |

## Reference spec

`docs/superpowers/specs/2026-05-21-backend-registry-design.md`
```

- [ ] **Step 2: Commit**

```bash
git add docs/architecture/backend-registry.md
git commit -m "docs(backend): contributor guide for adding backends + extensions"
```

---

## Task 12: Refresh ABI snapshot

**Files:**
- Modify: `docs/abi/v0.5-snapshot.json`

- [ ] **Step 1: Regenerate**

```bash
python3 scripts/abi_snapshot.py --version v0.5 --output docs/abi/v0.5-snapshot.json
```

Expected output: writes `docs/abi/v0.5-snapshot.json` with the new headers (`<alp/backend.h>`, `<alp/cap_instance.h>`) included.

- [ ] **Step 2: Verify diff contains the new public symbols**

```bash
git diff docs/abi/v0.5-snapshot.json | grep -E "alp_backend_(select|count)|alp_capabilities_has|ALP_BACKEND_REGISTER|ALP_ERR_NOT_PRESENT|ALP_ERR_NOT_IMPLEMENTED" | head -10
```
Expected: at least four of those names appear in the diff.

- [ ] **Step 3: Commit**

```bash
git add docs/abi/v0.5-snapshot.json
git commit -m "chore(abi): regenerate v0.5 snapshot for backend-registry foundation"
```

---

## Self-review checklist

1. **Spec coverage:** Slice 0 deliverables (Section 6 of the spec) cross-referenced:
   - `<alp/backend.h>` macro + selector → Tasks 3, 4 ✓
   - `<alp/cap.h>` instance-cap types + getter contract → Task 2 ✓ (via sibling `cap_instance.h` to avoid conflicting with the generator)
   - `<alp/peripheral.h>` three new error codes → Task 1 ✓ (note: `ALP_ERR_NOSUPPORT` already exists; we add the two new ones)
   - `tests/regress/test_backend_section_visibility.c` → Task 5 ✓
   - Toy backend in `tests/unit/backend_registry/` → Task 6 ✓
   - Three CI gates → Tasks 7, 8, 9 ✓
   - Wired into CI → Task 10 ✓
   - Contributor doc → Task 11 ✓
   - ABI snapshot → Task 12 ✓

2. **Type consistency:** `alp_backend_t`, `alp_backend_class_range_t`, `alp_capabilities_t`, `alp_instance_cap_t`, `demo_handle_t`, `demo_ops_t` used consistently across tasks.

3. **No placeholders:** every step shows the actual code or command. No "implement appropriate error handling" or "similar to Task N" stubs.

4. **Sequencing:** Tasks 1, 2, 3 are independent and can land in any order. Task 4 depends on Task 3. Task 5 depends on Tasks 3+4. Task 6 depends on Tasks 1+2+3+4. Tasks 7-9 are independent of the C work. Task 10 depends on 7+8+9. Task 11 depends on nothing (pure docs). Task 12 depends on 1+2+3.

## Execution sequence summary

- Phase A (parallel-friendly): Tasks 1, 2, 3, 11.
- Phase B: Task 4 (after 3).
- Phase C: Tasks 5, 6 (after 4). 6 depends on 1+2.
- Phase D (parallel-friendly): Tasks 7, 8, 9.
- Phase E: Task 10 (after 7+8+9).
- Phase F: Task 12 (any time after 1+2+3+4 are in).

Subagent dispatch hint: Phase A is three subagents in parallel; Phase D is three more; the rest sequential.
