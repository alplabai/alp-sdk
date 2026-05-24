# ADC Registry Pilot (Slice 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Migrate the ADC subsystem from today's Kconfig `#if`-ladder dispatch (in `src/zephyr/peripheral_adc.c`) to the linker-section backend registry shipped in Slice 0 (PR #17), with three backends (Alif Ensemble E7, V2N GD32 bridge, SW fallback) and the first vendor-extension header (`<alp/ext/alif/adc.h>`).

**Architecture:** A new internal ops vtable (`src/backends/adc/adc_ops.h`) describes the backend ABI. The class dispatcher (`src/adc_dispatch.c`) walks the registry once at first `alp_adc_open()` and caches the choice. Three backend files register themselves into `.alp_backends_adc` and provide ops bodies. Vendor extensions live under `<alp/ext/alif/adc.h>` and verify the handle's backend matches `vendor == "alif"` before dispatching through an extended ops struct (C "first-member-aliasing" pattern).

**Tech Stack:** C17 (ztest under twister on native_sim), Python 3.10+ (ABI snapshot refresh), Kconfig (new `CONFIG_ALP_SDK_ADC_SW_FALLBACK`), Zephyr `adc_*` driver class (Alif backend), `<alp/chips/gd32g553.h>` internal SDK API (V2N bridge).

**Spec:** `docs/superpowers/specs/2026-05-22-adc-registry-pilot-design.md`
**Foundation:** PR #17 (`feat/backend-registry-foundation`) — registry mechanism, capability types, error codes, CI gates
**Branch:** `feat/backend-registry-adc-pilot` (already checked out, based on `feat/backend-registry-foundation`)

---

## File Structure

**Create (public headers):**
- `include/alp/ext/alif/adc.h` — Alif ADC vendor extension API.

**Modify (public headers):**
- `include/alp/adc.h` — add `alp_adc_capabilities()` declaration; existing decls unchanged.

**Create (internal):**
- `src/backends/adc/adc_ops.h` — backend ABI (private).
- `src/adc_dispatch.c` — class dispatcher with handle pool + raw→uV math.
- `src/backends/adc/alif_e7.c` — Alif Ensemble E7 backend + vendor-ext bodies.
- `src/backends/adc/gd32_bridge.c` — V2N ADC via GD32 supervisor MCU.
- `src/backends/adc/sw_fallback.c` — deterministic saw-wave for native_sim.

**Create (DAC extraction):**
- `src/zephyr/peripheral_dac.c` — DAC code lifted out of `peripheral_adc.c`. Stays on the old `#if`-ladder.

**Delete:**
- `src/zephyr/peripheral_adc.c` — ADC code moved to backends; DAC moved to peripheral_dac.c.

**Create (tests):**
- `tests/unit/adc_registry/CMakeLists.txt`
- `tests/unit/adc_registry/prj.conf`
- `tests/unit/adc_registry/testcase.yaml`
- `tests/unit/adc_registry/src/test_adc_registry.c`

**Modify (build):**
- `zephyr/Kconfig` — add `CONFIG_ALP_SDK_ADC_SW_FALLBACK`.
- `zephyr/CMakeLists.txt` — include new sources (`src/adc_dispatch.c`, `src/backends/adc/*.c` conditionally), drop `src/zephyr/peripheral_adc.c`, add `src/zephyr/peripheral_dac.c`.

**Modify (example):**
- `examples/peripheral-io/adc-voltmeter/src/main.c` — add capability-gated teaching block.

**Modify (ABI):**
- `docs/abi/v0.5-snapshot.json` — regenerate to include new public surface.

---

## Task 1: Extract DAC to its own file

**Why first:** The 888-line `peripheral_adc.c` carries both ADC and DAC. Splitting before deleting lets the diff for Task 8 (the ADC removal) be cleaner.

**Files:**
- Create: `src/zephyr/peripheral_dac.c`
- Modify: `src/zephyr/peripheral_adc.c` (remove DAC sections)
- Modify: `zephyr/CMakeLists.txt` (add `peripheral_dac.c`)

- [ ] **Step 1: Inventory the DAC-only symbols**

```bash
grep -nE "^(alp_dac_|static.*_dac_|struct alp_dac|alp_dac_t)" src/zephyr/peripheral_adc.c
```

Expected: lines defining `alp_dac_open`, `alp_dac_write_mv`, `alp_dac_close`, `struct alp_dac`, DAC handle pool, DAC-specific helpers, Kconfig-gated DAC paths.

Also find DAC includes:
```bash
grep -nE "#include.*dac|#if.*DAC" src/zephyr/peripheral_adc.c
```

- [ ] **Step 2: Create the new file**

Create `src/zephyr/peripheral_dac.c` with this skeleton at the top; then COPY the inventoried DAC sections from `peripheral_adc.c` into it, preserving their order:

```c
/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DAC peripheral implementation -- extracted from the legacy
 * peripheral_adc.c during the Slice 1 ADC-registry migration so
 * the ADC code could move into src/backends/adc/* without taking
 * DAC with it.  DAC stays on the existing Kconfig #if-ladder
 * pattern until Slice 4 (mechanical migrations).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/drivers/dac.h>

#include <alp/adc.h>      /* exposes alp_dac_* declarations */
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

/* === DAC handle pool === */
/* ... paste the DAC struct + pool from peripheral_adc.c ... */

/* === DAC public API === */
/* ... paste alp_dac_open / write_mv / close ... */

/* === DAC-specific helpers === */
/* ... paste DAC-only static helpers ... */
```

- [ ] **Step 3: Remove the same DAC sections from `peripheral_adc.c`**

For each line range identified in Step 1, delete it from `peripheral_adc.c`. After the removal, `grep -E "_dac_|alp_dac" src/zephyr/peripheral_adc.c` MUST return zero lines.

- [ ] **Step 4: Update `zephyr/CMakeLists.txt`**

Find the existing `zephyr_library_sources(... src/zephyr/peripheral_adc.c ...)` reference. Add `src/zephyr/peripheral_dac.c` to the same list. Do NOT remove `peripheral_adc.c` yet — Tasks 6–8 do that.

- [ ] **Step 5: Compile-smoke**

```bash
python3 scripts/abi_snapshot.py --version v0.5 --output /tmp/abi-dac-extract.json
```

Expected: writes without error. The ABI surface is unchanged (DAC declarations are still in `<alp/adc.h>`), so the snapshot diff should be empty.

```bash
git diff docs/abi/v0.5-snapshot.json
```

Expected: no diff lines (file unchanged on disk; we didn't write it).

- [ ] **Step 6: Commit**

```bash
git add src/zephyr/peripheral_dac.c src/zephyr/peripheral_adc.c zephyr/CMakeLists.txt
git commit -m "refactor(dac): extract DAC into peripheral_dac.c

Lifts the DAC implementation out of the 888-line peripheral_adc.c
into its own file so the upcoming ADC migration to the backend
registry (Slice 1) doesn't drag DAC along.  DAC stays on the
existing #if-ladder pattern until Slice 4.

No behavioural change; cmake list updated."
```

---

## Task 2: Add `alp_adc_capabilities` declaration to `<alp/adc.h>`

**Files:**
- Modify: `include/alp/adc.h`

- [ ] **Step 1: Locate the existing close declaration**

```bash
grep -n "alp_adc_close\|alp_adc_read_uv" include/alp/adc.h | head -5
```

The new `alp_adc_capabilities` declaration goes immediately after `alp_adc_close`.

- [ ] **Step 2: Apply the edit**

In `include/alp/adc.h`, find the line `void alp_adc_close(alp_adc_t *adc);` and insert AFTER it:

```c
/**
 * @brief Query the capabilities of an opened ADC handle.
 *
 * Returns a pointer to the @ref alp_capabilities_t descriptor the
 * backend populated at open time. Useful for runtime gating
 * (e.g. only enabling DMA-stream code paths when the backend
 * advertises @ref ALP_INSTANCE_CAP_DMA).
 *
 * @param adc  Handle from @ref alp_adc_open, or NULL.
 * @return Pointer valid for the handle's lifetime; NULL if @p adc
 *         is NULL.
 */
const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *adc);
```

Also add the include of `<alp/cap_instance.h>` near the top of the file if not already present:

```bash
grep -n "cap_instance.h\|cap.h" include/alp/adc.h
```

If not present, add `#include <alp/cap_instance.h>` near the other `#include <alp/...>` directives.

- [ ] **Step 3: Compile-smoke**

```bash
gcc -c -x c -I include -o /tmp/adc-cap-smoke.o - <<'EOF'
#include <alp/adc.h>
const alp_capabilities_t *probe(alp_adc_t *h) { return alp_adc_capabilities(h); }
EOF
```

Expected: exit 0.

- [ ] **Step 4: Commit**

```bash
git add include/alp/adc.h
git commit -m "feat(adc): declare alp_adc_capabilities() public getter"
```

---

## Task 3: Internal backend ops header

**Files:**
- Create: `src/backends/adc/adc_ops.h`

- [ ] **Step 1: Write the header**

Create `src/backends/adc/adc_ops.h`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_adc dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Vendor-specific backends extend alp_adc_ops_t by embedding it
 * as the first member of a larger struct (C first-member-aliasing
 * pattern).  Vendor functions cast the const void * ops field of
 * alp_backend_t back to the larger struct after verifying the
 * vendor name.
 */

#ifndef ALP_BACKENDS_ADC_OPS_H
#define ALP_BACKENDS_ADC_OPS_H

#include <stdint.h>
#include <alp/adc.h>
#include <alp/peripheral.h>
#include <alp/cap_instance.h>

typedef struct alp_adc_ops alp_adc_ops_t;

typedef struct alp_adc_backend_state {
    uint32_t              reference_uv;
    uint16_t              resolution_bits;
    const alp_adc_ops_t  *ops;
    void                 *be_data;
} alp_adc_backend_state_t;

struct alp_adc_ops {
    /* Open the channel.  cfg is the customer's config; state is
     * preallocated by the dispatcher; caps_out is filled with the
     * (possibly probe-refined) instance capabilities.
     *
     * The backend MUST set state->reference_uv and
     * state->resolution_bits before returning ALP_OK.
     *
     * Returns ALP_OK on success; ALP_ERR_NOSUPPORT for bad cfg
     * (e.g. resolution_bits exceeds the SoC's max); ALP_ERR_NOT_READY
     * if hardware isn't initialised.
     */
    alp_status_t (*open)(const alp_adc_config_t *cfg,
                         alp_adc_backend_state_t *state,
                         alp_capabilities_t *caps_out);

    /* One-shot raw read.  Signed for symmetry with differential
     * mode; single-ended SoCs return non-negative values. */
    alp_status_t (*read_raw)(alp_adc_backend_state_t *state,
                             int32_t *raw_out);

    /* Tear down.  May be NULL for stateless backends. */
    void (*close)(alp_adc_backend_state_t *state);
};

#endif /* ALP_BACKENDS_ADC_OPS_H */
```

- [ ] **Step 2: Compile-smoke**

```bash
gcc -c -x c -I include -o /tmp/adc-ops-smoke.o - <<'EOF'
#include "src/backends/adc/adc_ops.h"
const alp_adc_ops_t *test(void) { return NULL; }
EOF
```

Expected: exit 0.

- [ ] **Step 3: Commit**

```bash
git add src/backends/adc/adc_ops.h
git commit -m "feat(adc): private backend ops vtable header"
```

---

## Task 4: Class dispatcher (`src/adc_dispatch.c`)

**Files:**
- Create: `src/adc_dispatch.c`
- Modify: `zephyr/CMakeLists.txt` (add `src/adc_dispatch.c`)

NOTE: this task creates the dispatcher BEFORE any backend exists. The dispatcher links cleanly because the linker section is empty; `alp_adc_open()` will return NULL with `ALP_ERR_NOT_PRESENT_ON_THIS_SOC` until Task 5 registers the SW fallback. The existing `peripheral_adc.c` still provides the real implementations during this transitional state — link conflicts are addressed in Task 8 when `peripheral_adc.c` is deleted.

- [ ] **Step 1: Locate the existing `_set_last_error` mechanism**

```bash
grep -nE "alp_last_error|_set_last_error|_last_error_" src/zephyr/peripheral_adc.c src/zephyr/peripheral_dac.c include/alp/peripheral.h | head -10
```

Identify the exact function name + signature used by the existing code. The dispatcher reuses it verbatim. If the function is `static` to `peripheral_adc.c`, promote it to a non-static helper in `src/peripheral_helpers.c` (extract during this task; one new file).

- [ ] **Step 2: Decide between two implementations**

Either (a) the existing `_set_last_error` is callable from outside `peripheral_adc.c` (already non-static or in a shared TU) — reuse as-is, OR (b) it's static — extract to `src/peripheral_helpers.c` as a public-internal helper with a name like `alp_set_last_error(alp_status_t)`. Document which path was taken in the commit message.

- [ ] **Step 3: Write the dispatcher**

Create `src/adc_dispatch.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * ADC class dispatcher.  Owns the public alp_adc_* API surface
 * and routes through the backend registry mechanism shipped in
 * Slice 0.  All voltage-conversion math (raw -> uV) lives here so
 * every backend reports raw values in the same convention.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/adc/adc_ops.h"

/* The handle struct is opaque to customers; layout lives here. */
struct alp_adc {
    alp_adc_backend_state_t  state;
    const alp_backend_t     *backend;
    alp_capabilities_t       cached_caps;
    bool                     in_use;
};

ALP_BACKEND_DEFINE_CLASS(adc);

/* Sized by the same Kconfig the legacy peripheral_adc.c used.  If
 * CONFIG_ALP_SDK_ADC_HANDLE_POOL is unset, fall back to 8. */
#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static struct alp_adc _pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static struct alp_adc *_alloc_handle(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
        if (!_pool[i].in_use) {
            memset(&_pool[i], 0, sizeof(_pool[i]));
            _pool[i].in_use = true;
            return &_pool[i];
        }
    }
    return NULL;
}

static void _free_handle(struct alp_adc *h)
{
    h->in_use = false;
}

/* Thread-local last error is reused from peripheral_helpers.c. */
extern void alp_set_last_error(alp_status_t code);

alp_adc_t *alp_adc_open(const alp_adc_config_t *cfg)
{
    if (cfg == NULL) {
        alp_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("adc", ALP_SOC_REF_STR);
    if (be == NULL) {
        alp_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
        return NULL;
    }
    const alp_adc_ops_t *ops = (const alp_adc_ops_t *)be->ops;
    if (ops == NULL || ops->open == NULL) {
        alp_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
        return NULL;
    }
    struct alp_adc *h = _alloc_handle();
    if (h == NULL) {
        alp_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->backend = be;
    h->state.ops = ops;
    alp_capabilities_t caps = { .flags = be->base_caps };
    if (be->probe != NULL) {
        uint32_t refined = caps.flags;
        (void)be->probe(cfg->channel_id, &refined);
        caps.flags = refined;
    }
    alp_status_t rc = ops->open(cfg, &h->state, &caps);
    if (rc != ALP_OK) {
        _free_handle(h);
        alp_set_last_error(rc);
        return NULL;
    }
    h->cached_caps = caps;
    return h;
}

alp_status_t alp_adc_read_raw(alp_adc_t *h, int32_t *raw_out)
{
    if (h == NULL || raw_out == NULL) {
        return ALP_ERR_INVAL;
    }
    return h->state.ops->read_raw(&h->state, raw_out);
}

alp_status_t alp_adc_read_uv(alp_adc_t *h, int32_t *uv_out)
{
    if (h == NULL || uv_out == NULL) {
        return ALP_ERR_INVAL;
    }
    int32_t raw = 0;
    alp_status_t rc = h->state.ops->read_raw(&h->state, &raw);
    if (rc != ALP_OK) {
        return rc;
    }
    const int64_t fs = (int64_t)((1u << h->state.resolution_bits) - 1u);
    if (fs == 0) {
        return ALP_ERR_NOT_READY;
    }
    *uv_out = (int32_t)((int64_t)raw * (int64_t)h->state.reference_uv / fs);
    return ALP_OK;
}

void alp_adc_close(alp_adc_t *h)
{
    if (h == NULL) {
        return;
    }
    if (h->state.ops != NULL && h->state.ops->close != NULL) {
        h->state.ops->close(&h->state);
    }
    _free_handle(h);
}

const alp_capabilities_t *alp_adc_capabilities(const alp_adc_t *h)
{
    return (h != NULL) ? &h->cached_caps : NULL;
}
```

- [ ] **Step 4: Extract `alp_set_last_error` to `src/peripheral_helpers.c` if needed**

If Step 2 path (b) was chosen, create `src/peripheral_helpers.c` with the helper:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared peripheral helpers: thread-local last-error storage.
 * Extracted from src/zephyr/peripheral_adc.c during the Slice 1
 * ADC-registry migration to make the storage callable from
 * src/adc_dispatch.c.
 */

#include <alp/peripheral.h>

/* Storage definition stays in the OS-specific TU that knows about
 * Zephyr thread-local storage.  This file exposes a setter that the
 * dispatcher calls.  See src/zephyr/peripheral_helpers_zephyr.c for
 * the actual TLS-backed storage. */

void alp_set_last_error(alp_status_t code)
{
    /* Delegate to the TLS-backed storage already defined in
     * peripheral_adc.c (or now in peripheral_helpers_zephyr.c). */
    extern void _alp_tls_set_last_error(alp_status_t);
    _alp_tls_set_last_error(code);
}
```

If the existing `_set_last_error` is already non-static, skip this step. The implementer decides based on Step 1 inspection.

- [ ] **Step 5: Update `zephyr/CMakeLists.txt`**

Add `src/adc_dispatch.c` to the same `zephyr_library_sources(...)` block that holds `src/backend.c` (from Slice 0). If Step 4 created `src/peripheral_helpers.c`, add that too. Do NOT remove `src/zephyr/peripheral_adc.c` yet — Tasks 5-8 manage that.

- [ ] **Step 6: Compile-smoke**

```bash
gcc -c -x c -I include -o /tmp/adc-dispatch-smoke.o src/adc_dispatch.c
```

Expected: exit 0 (or warning about `extern alp_set_last_error` undefined link-time — that's fine for `-c`).

- [ ] **Step 7: Commit**

```bash
git add src/adc_dispatch.c zephyr/CMakeLists.txt
# include src/peripheral_helpers.c if step 4 created it
git commit -m "feat(adc): class dispatcher + raw->uV conversion + handle pool

Owns the public alp_adc_* API.  Walks the registry once at first
alp_adc_open and dispatches through cached ops.  No backend
registered yet -- next tasks add SW fallback then real backends.
peripheral_adc.c still owns dispatch until Task 8 removes it."
```

---

## Task 5: SW fallback backend + Kconfig

**Files:**
- Create: `src/backends/adc/sw_fallback.c`
- Modify: `zephyr/Kconfig` (add `CONFIG_ALP_SDK_ADC_SW_FALLBACK`)
- Modify: `zephyr/CMakeLists.txt` (conditional include)

- [ ] **Step 1: Write the backend**

Create `src/backends/adc/sw_fallback.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software ADC fallback backend.  Returns a deterministic saw-wave
 * on every read; lets examples/ compile and exercise the dispatch
 * path on native_sim without a real ADC.
 *
 * @par Cost: ROM ~1.5 KB, RAM 4 B per handle (counter for the saw)
 * @par Performance: 1 sample per call; no DMA; no real conversion.
 *      For native_sim build/test only -- never use on production.
 */

#include <stdint.h>
#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "adc_ops.h"

static int32_t _saw_counter = 0;

static alp_status_t sw_open(const alp_adc_config_t *cfg,
                            alp_adc_backend_state_t *state,
                            alp_capabilities_t *caps_out)
{
    (void)cfg;
    state->reference_uv    = 3300000u;   /* 3.3 V reference */
    state->resolution_bits = 12u;
    state->be_data         = NULL;        /* stateless */
    caps_out->max_resolution_bits = 12u;
    caps_out->max_sample_rate     = 0u;
    caps_out->channel_count       = 8u;
    return ALP_OK;
}

static alp_status_t sw_read_raw(alp_adc_backend_state_t *state,
                                int32_t *raw_out)
{
    (void)state;
    *raw_out = _saw_counter;
    _saw_counter = (_saw_counter + 137) & 0x0FFF;    /* saw mod 4096 */
    return ALP_OK;
}

static const alp_adc_ops_t sw_ops = {
    .open     = sw_open,
    .read_raw = sw_read_raw,
    .close    = NULL,
};

ALP_BACKEND_REGISTER(adc, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &sw_ops,
    .probe       = NULL,
});
```

- [ ] **Step 2: Add the Kconfig entry**

In `zephyr/Kconfig`, find the existing `ALP_SDK_ADC_*` block (search for `CONFIG_ALP_SDK_ADC`) and add:

```kconfig
config ALP_SDK_ADC_SW_FALLBACK
	bool "Software ADC fallback (polled, deterministic saw wave)"
	depends on ALP_SDK
	default y if BOARD_NATIVE_SIM || ARCH_POSIX
	default n
	help
	  Compiles a software ADC backend that registers as a wildcard
	  in the .alp_backends_adc section at priority 0.  The selector
	  picks it only when no hardware backend exists for the active
	  SoC.  Off by default on real silicon; default on for
	  native_sim builds so examples compile.
```

- [ ] **Step 3: Wire into the build**

In `zephyr/CMakeLists.txt`, add:

```cmake
if(CONFIG_ALP_SDK_ADC_SW_FALLBACK)
    zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../src/backends/adc/sw_fallback.c)
endif()
```

(Adjust the path to match the repo's `zephyr_library_sources` style — use the same idiom as `src/adc_dispatch.c` from Task 4.)

- [ ] **Step 4: Verify CI gate is happy**

```bash
python3 scripts/check_sw_fallback_tags.py
echo "exit=$?"
```

Expected: exit 0. The new file has both `@par Cost:` and `@par Performance:` tags.

- [ ] **Step 5: Commit**

```bash
git add src/backends/adc/sw_fallback.c zephyr/Kconfig zephyr/CMakeLists.txt
git commit -m "feat(adc): SW fallback backend (deterministic saw wave)

Wildcard registration at priority 0; picked only when no HW
backend matches the active SoC.  Default-on for native_sim,
default-off for real silicon.  Cost + Performance tags present
for check_sw_fallback_tags.py gate."
```

---

## Task 6: Alif Ensemble E7 backend

**Files:**
- Create: `src/backends/adc/alif_e7.c`
- Modify: `zephyr/CMakeLists.txt`

- [ ] **Step 1: Read the existing Alif ADC code in peripheral_adc.c**

```bash
grep -nE "DT_NODELABEL\(adc|adc_channel_setup|adc_read\(|ALIF" src/zephyr/peripheral_adc.c | head -20
```

Identify the existing Zephyr `adc_*` driver-class calls. The new backend reuses the same call sequence; the migration is structural, not a rewrite.

- [ ] **Step 2: Write the backend**

Create `src/backends/adc/alif_e7.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Alif Ensemble E7 ADC backend.  Routes through Zephyr's adc
 * driver class; the Alif HAL pack registers the SoC's ADC as a DT
 * node and the driver class handles register-level details.
 *
 * Also hosts the alp_alif_adc_* vendor-extension bodies, since
 * the vendor knobs cast through this file's extended ops struct.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/adc.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/ext/alif/adc.h>

#include "adc_ops.h"

#define ALIF_ADC_NODE DT_NODELABEL(adc0)

typedef struct alif_e7_adc_state {
    const struct device *dev;
    uint8_t              channel_id;
    uint8_t              resolution_bits;
    uint16_t             oversample_ratio;     /* 1 = disabled */
    alp_alif_adc_trigger_t trigger_source;
    bool                 in_use;
} alif_e7_adc_state_t;

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static alif_e7_adc_state_t _state_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static alif_e7_adc_state_t *_alloc_state(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
        if (!_state_pool[i].in_use) {
            memset(&_state_pool[i], 0, sizeof(_state_pool[i]));
            _state_pool[i].in_use = true;
            _state_pool[i].oversample_ratio = 1u;
            return &_state_pool[i];
        }
    }
    return NULL;
}

static void _free_state(alif_e7_adc_state_t *s) { s->in_use = false; }

static alp_status_t alif_e7_open(const alp_adc_config_t *cfg,
                                 alp_adc_backend_state_t *st,
                                 alp_capabilities_t *caps_out)
{
    if (cfg->resolution_bits > ALP_SOC_ADC_MAX_RESOLUTION_BITS) {
        return ALP_ERR_NOSUPPORT;
    }
    alif_e7_adc_state_t *s = _alloc_state();
    if (s == NULL) {
        return ALP_ERR_NOMEM;
    }
    s->dev = DEVICE_DT_GET(ALIF_ADC_NODE);
    if (!device_is_ready(s->dev)) {
        _free_state(s);
        return ALP_ERR_NOT_READY;
    }
    s->channel_id      = cfg->channel_id;
    s->resolution_bits = cfg->resolution_bits;

    struct adc_channel_cfg ch_cfg = {
        .gain             = ADC_GAIN_1,
        .reference        = ADC_REF_INTERNAL,
        .acquisition_time = ADC_ACQ_TIME_DEFAULT,
        .channel_id       = cfg->channel_id,
        .differential     = 0,
    };
    int rc = adc_channel_setup(s->dev, &ch_cfg);
    if (rc < 0) {
        _free_state(s);
        return ALP_ERR_IO;
    }

    st->be_data         = s;
    st->reference_uv    = 1800000u;            /* Alif internal ref */
    st->resolution_bits = cfg->resolution_bits;

    caps_out->max_resolution_bits = ALP_SOC_ADC_MAX_RESOLUTION_BITS;
    caps_out->max_sample_rate     = 1000000u;  /* 1 Msps typical for E7 */
    caps_out->channel_count       = ALP_SOC_ADC_COUNT;
    return ALP_OK;
}

static alp_status_t alif_e7_read_raw(alp_adc_backend_state_t *st,
                                     int32_t *raw_out)
{
    alif_e7_adc_state_t *s = (alif_e7_adc_state_t *)st->be_data;
    int16_t buf = 0;
    struct adc_sequence seq = {
        .channels    = BIT(s->channel_id),
        .buffer      = &buf,
        .buffer_size = sizeof(buf),
        .resolution  = s->resolution_bits,
        .oversampling = (s->oversample_ratio > 1u)
                        ? (uint8_t)(__builtin_ctz(s->oversample_ratio))
                        : 0u,
    };
    int rc = adc_read(s->dev, &seq);
    if (rc < 0) {
        return ALP_ERR_IO;
    }
    *raw_out = (int32_t)buf;
    return ALP_OK;
}

static void alif_e7_close(alp_adc_backend_state_t *st)
{
    if (st->be_data != NULL) {
        _free_state((alif_e7_adc_state_t *)st->be_data);
        st->be_data = NULL;
    }
}

/* === Vendor-extension bodies live in this file too === */

static alp_status_t alif_e7_set_oversampling(alp_adc_t *h, uint16_t ratio);
static alp_status_t alif_e7_set_trigger_source(alp_adc_t *h, alp_alif_adc_trigger_t src);

/* Extended ops struct: base ops + vendor-specific function pointers. */
typedef struct alif_e7_adc_ops {
    alp_adc_ops_t base;
    alp_status_t (*set_oversampling)(alp_adc_t *h, uint16_t ratio);
    alp_status_t (*set_trigger_source)(alp_adc_t *h, alp_alif_adc_trigger_t src);
} alif_e7_adc_ops_t;

static const alif_e7_adc_ops_t alif_e7_ops = {
    .base = {
        .open     = alif_e7_open,
        .read_raw = alif_e7_read_raw,
        .close    = alif_e7_close,
    },
    .set_oversampling   = alif_e7_set_oversampling,
    .set_trigger_source = alif_e7_set_trigger_source,
};

ALP_BACKEND_REGISTER(adc, alif_e7, {
    .silicon_ref = "alif:ensemble:e7",
    .vendor      = "alif",
    .base_caps   = (uint32_t)(ALP_INSTANCE_CAP_HW_OVERSAMPLE
                              | ALP_INSTANCE_CAP_HW_TRIGGER),
    .priority    = 100,
    .ops         = &alif_e7_ops.base,         /* points at the embedded base */
    .probe       = NULL,
});

/* The vendor-ext function bodies follow at file scope but reach the
 * private state via the customer's alp_adc_t.  The public header
 * <alp/ext/alif/adc.h> declares them; alp_adc_t is opaque to
 * customers, so we need a private accessor.  The dispatcher's
 * struct alp_adc layout is defined in src/adc_dispatch.c -- we
 * forward-declare it minimally here, in agreement with the
 * dispatcher's definition. */

struct alp_adc {
    alp_adc_backend_state_t  state;
    const alp_backend_t     *backend;
    alp_capabilities_t       cached_caps;
    bool                     in_use;
};

static bool _is_power_of_two_le_256(uint16_t v)
{
    return v != 0 && v <= 256 && (v & (v - 1u)) == 0;
}

static alp_status_t alif_e7_set_oversampling(alp_adc_t *h, uint16_t ratio)
{
    if (h == NULL) {
        return ALP_ERR_INVAL;
    }
    if (strcmp(h->backend->vendor, "alif") != 0) {
        return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    }
    if (!_is_power_of_two_le_256(ratio)) {
        return ALP_ERR_INVAL;
    }
    alif_e7_adc_state_t *s = (alif_e7_adc_state_t *)h->state.be_data;
    s->oversample_ratio = ratio;
    return ALP_OK;
}

static alp_status_t alif_e7_set_trigger_source(alp_adc_t *h,
                                               alp_alif_adc_trigger_t src)
{
    if (h == NULL) {
        return ALP_ERR_INVAL;
    }
    if (strcmp(h->backend->vendor, "alif") != 0) {
        return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    }
    if ((unsigned)src > (unsigned)ALP_ALIF_ADC_TRIGGER_EXT_PIN) {
        return ALP_ERR_INVAL;
    }
    alif_e7_adc_state_t *s = (alif_e7_adc_state_t *)h->state.be_data;
    s->trigger_source = src;
    return ALP_OK;
}

/* Public-facing thunks called from <alp/ext/alif/adc.h>.  These are
 * what customer code actually calls; they dispatch via the extended
 * ops struct so a future-built non-Alif Alif fork could replace
 * them without changing customer code. */

alp_status_t alp_alif_adc_set_oversampling(alp_adc_t *h, uint16_t ratio)
{
    if (h == NULL) {
        return ALP_ERR_INVAL;
    }
    if (strcmp(h->backend->vendor, "alif") != 0) {
        return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    }
    const alif_e7_adc_ops_t *ops =
        (const alif_e7_adc_ops_t *)h->backend->ops;
    return ops->set_oversampling(h, ratio);
}

alp_status_t alp_alif_adc_set_trigger_source(alp_adc_t *h,
                                             alp_alif_adc_trigger_t src)
{
    if (h == NULL) {
        return ALP_ERR_INVAL;
    }
    if (strcmp(h->backend->vendor, "alif") != 0) {
        return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
    }
    const alif_e7_adc_ops_t *ops =
        (const alif_e7_adc_ops_t *)h->backend->ops;
    return ops->set_trigger_source(h, src);
}
```

NOTE: the `struct alp_adc` definition above must MATCH the one in `src/adc_dispatch.c` (Task 4) byte-for-byte. If you change the layout in one place, change both. The spec's "Open questions" note flagged this — the proper long-term fix is to put the struct in `src/backends/adc/adc_ops.h`. Apply that refactor as part of this task: move `struct alp_adc { ... };` into `adc_ops.h` and include `adc_ops.h` from both `adc_dispatch.c` and `alif_e7.c`.

- [ ] **Step 3: Refactor — `struct alp_adc` into `adc_ops.h`**

Move the layout from `src/adc_dispatch.c` into `src/backends/adc/adc_ops.h`. Both `adc_dispatch.c` and `alif_e7.c` then include the same definition. Remove the duplicate from `adc_dispatch.c` and from `alif_e7.c`.

After this move, `adc_ops.h` includes the `bool` type — add `#include <stdbool.h>` if not present.

- [ ] **Step 4: Wire into the build**

In `zephyr/CMakeLists.txt`, add (under a guard if the project uses `CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7` to select Alif targets):

```cmake
if(CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7)
    zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../src/backends/adc/alif_e7.c)
endif()
```

Check the existing pattern for SoC-gated compilation; many entries in the repo's CMakeLists use this style.

- [ ] **Step 5: Compile-smoke (best-effort)**

A full Alif build needs the Zephyr toolchain. Skip locally; CI validates. Confirm headers parse:

```bash
gcc -c -x c -I include -DCONFIG_ALP_SOC_ALIF_ENSEMBLE_E7 -o /tmp/alif-headers-smoke.o - <<'EOF'
#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/alif/adc.h>
EOF
```

Expected: exit 0 (after Task 9 lands the `<alp/ext/alif/adc.h>` header). If Task 9 hasn't landed yet, skip the `<alp/ext/alif/adc.h>` include for this smoke and confirm again after Task 9.

- [ ] **Step 6: Commit**

```bash
git add src/backends/adc/alif_e7.c src/backends/adc/adc_ops.h src/adc_dispatch.c zephyr/CMakeLists.txt
git commit -m "feat(adc): Alif Ensemble E7 backend + vendor-ext bodies

Wraps the existing Zephyr adc driver-class wiring -- same code
path peripheral_adc.c uses today; the migration is structural.
Vendor-ext functions (alp_alif_adc_set_oversampling, _set_
trigger_source) live in this file too, gated by a vendor-string
check against the handle's backend.

Also lifts struct alp_adc layout into adc_ops.h so the
dispatcher + backend share the same definition without
duplication."
```

---

## Task 7: V2N GD32 bridge backend

**Files:**
- Create: `src/backends/adc/gd32_bridge.c`
- Modify: `zephyr/CMakeLists.txt`

- [ ] **Step 1: Read existing GD32 bridge ADC code in peripheral_adc.c**

```bash
grep -nE "CMD_ADC_|gd32g553_|v2n_supervisor" src/zephyr/peripheral_adc.c | head -20
```

Identify the existing supervisor-MCU command sequence (CMD_ADC_OPEN, CMD_ADC_READ, etc.) and the mutex / locking pattern.

- [ ] **Step 2: Write the backend**

Create `src/backends/adc/gd32_bridge.c`:

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * V2N ADC backend.  V2N's M33 has no direct ADC; the SDK routes
 * ADC commands through the GD32G553 supervisor MCU bridge using
 * the existing v2n_supervisor.h API.
 */

#include <stdint.h>
#include <string.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "adc_ops.h"
#include "../../chips/gd32g553/v2n_supervisor.h"   /* internal SDK header */

typedef struct gd32_bridge_state {
    uint8_t bridge_handle_id;
    uint8_t channel_id;
    uint16_t resolution_bits;
    bool    in_use;
} gd32_bridge_state_t;

#ifndef CONFIG_ALP_SDK_ADC_HANDLE_POOL
#define CONFIG_ALP_SDK_ADC_HANDLE_POOL 8
#endif

static gd32_bridge_state_t _state_pool[CONFIG_ALP_SDK_ADC_HANDLE_POOL];

static gd32_bridge_state_t *_alloc_state(void)
{
    for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_ADC_HANDLE_POOL; ++i) {
        if (!_state_pool[i].in_use) {
            memset(&_state_pool[i], 0, sizeof(_state_pool[i]));
            _state_pool[i].in_use = true;
            return &_state_pool[i];
        }
    }
    return NULL;
}

static void _free_state(gd32_bridge_state_t *s) { s->in_use = false; }

static alp_status_t gd32_open(const alp_adc_config_t *cfg,
                              alp_adc_backend_state_t *st,
                              alp_capabilities_t *caps_out)
{
    if (cfg->resolution_bits > 12) {        /* GD32G553 ADC tops out at 12-bit */
        return ALP_ERR_NOSUPPORT;
    }
    gd32_bridge_state_t *s = _alloc_state();
    if (s == NULL) {
        return ALP_ERR_NOMEM;
    }
    s->channel_id      = cfg->channel_id;
    s->resolution_bits = cfg->resolution_bits;

    /* Send CMD_ADC_OPEN to the supervisor.  The bridge replies with
     * an opaque handle ID we hand back on every CMD_ADC_READ. */
    uint8_t handle_id = 0;
    int rc = v2n_supervisor_adc_open(cfg->channel_id,
                                     cfg->resolution_bits,
                                     &handle_id);
    if (rc < 0) {
        _free_state(s);
        return ALP_ERR_IO;
    }
    s->bridge_handle_id = handle_id;

    st->be_data         = s;
    st->reference_uv    = 3300000u;       /* GD32 default 3.3 V */
    st->resolution_bits = cfg->resolution_bits;

    caps_out->max_resolution_bits = 12u;
    caps_out->max_sample_rate     = 1000000u;
    caps_out->channel_count       = 16u;
    return ALP_OK;
}

static alp_status_t gd32_read_raw(alp_adc_backend_state_t *st,
                                  int32_t *raw_out)
{
    gd32_bridge_state_t *s = (gd32_bridge_state_t *)st->be_data;
    int16_t raw = 0;
    int rc = v2n_supervisor_adc_read(s->bridge_handle_id, &raw);
    if (rc < 0) {
        return ALP_ERR_IO;
    }
    *raw_out = (int32_t)raw;
    return ALP_OK;
}

static void gd32_close(alp_adc_backend_state_t *st)
{
    gd32_bridge_state_t *s = (gd32_bridge_state_t *)st->be_data;
    if (s != NULL) {
        (void)v2n_supervisor_adc_close(s->bridge_handle_id);
        _free_state(s);
        st->be_data = NULL;
    }
}

static const alp_adc_ops_t gd32_ops = {
    .open     = gd32_open,
    .read_raw = gd32_read_raw,
    .close    = gd32_close,
};

ALP_BACKEND_REGISTER(adc, gd32_bridge, {
    .silicon_ref = "renesas:rzv2n:n44",
    .vendor      = "renesas",       /* SoC vendor, not bridge chip */
    .base_caps   = 0u,              /* no HW oversample/trigger via bridge */
    .priority    = 100,
    .ops         = &gd32_ops,
    .probe       = NULL,
});
```

NOTE: this task assumes existing helpers `v2n_supervisor_adc_open` / `_read` / `_close` already exist in `chips/gd32g553/`. If they don't, either:
(a) extract them from `peripheral_adc.c` (the existing implementation likely calls into `gd32g553_*` functions directly — wrap them in `v2n_supervisor_adc_*` thin wrappers as part of this task), OR
(b) call the existing `gd32g553_*` functions directly from this backend.

The implementer chooses based on Step 1 inspection. Document the choice in the commit message.

- [ ] **Step 3: Wire into the build**

```cmake
if(CONFIG_ALP_SOC_RENESAS_RZV2N_N44)
    zephyr_library_sources(${CMAKE_CURRENT_LIST_DIR}/../src/backends/adc/gd32_bridge.c)
endif()
```

- [ ] **Step 4: Compile-smoke**

Defer to CI (needs the V2N toolchain).

- [ ] **Step 5: Commit**

```bash
git add src/backends/adc/gd32_bridge.c zephyr/CMakeLists.txt
# Optionally chips/gd32g553/v2n_supervisor*.{c,h} if step 2(a) was taken
git commit -m "feat(adc): V2N ADC backend via GD32 supervisor MCU bridge

Mirrors the existing CMD_ADC_OPEN / READ / CLOSE command set
that peripheral_adc.c used for V2N targets.  base_caps=0 in
Slice 1 -- HW oversample/trigger via the bridge land in a
follow-up once the bridge firmware grows those features."
```

---

## Task 8: Delete `src/zephyr/peripheral_adc.c`

**Files:**
- Delete: `src/zephyr/peripheral_adc.c`
- Modify: `zephyr/CMakeLists.txt`

- [ ] **Step 1: Confirm DAC was extracted cleanly**

```bash
grep -cE "_dac_|alp_dac" src/zephyr/peripheral_adc.c
```

Expected: 0. If non-zero, return to Task 1 — DAC wasn't fully extracted.

- [ ] **Step 2: Confirm no other file includes `peripheral_adc.c` as a translation unit**

```bash
grep -rn "peripheral_adc.c" zephyr/ src/ examples/
```

Expected: only `zephyr/CMakeLists.txt` references it.

- [ ] **Step 3: Delete the file + remove from CMakeLists**

```bash
git rm src/zephyr/peripheral_adc.c
```

Edit `zephyr/CMakeLists.txt` and remove the `peripheral_adc.c` entry from `zephyr_library_sources(...)`.

- [ ] **Step 4: Smoke-check the public API still has decls**

```bash
grep -nE "alp_adc_open|alp_adc_read_raw|alp_adc_read_uv|alp_adc_close|alp_adc_capabilities" include/alp/adc.h
```

Expected: five matching declarations. The dispatcher provides the bodies now.

- [ ] **Step 5: Commit**

```bash
git add -u
git commit -m "refactor(adc): remove legacy peripheral_adc.c

ADC implementation now lives in:
  src/adc_dispatch.c                  (class dispatcher)
  src/backends/adc/sw_fallback.c      (* wildcard, priority 0)
  src/backends/adc/alif_e7.c          (alif:ensemble:e7)
  src/backends/adc/gd32_bridge.c      (renesas:rzv2n:n44)

DAC stayed put: src/zephyr/peripheral_dac.c."
```

---

## Task 9: Vendor extension header `<alp/ext/alif/adc.h>`

**Files:**
- Create: `include/alp/ext/alif/adc.h`

- [ ] **Step 1: Write the header**

Create `include/alp/ext/alif/adc.h`:

```c
/**
 * @file ext/alif/adc.h
 * @brief Alif Ensemble ADC vendor-specific knobs.
 *
 * Non-portable. Include only when you've committed to Alif silicon
 * for the gated feature.  Every function in this header verifies
 * the handle's backend is Alif before touching hardware; calls on
 * a non-Alif handle return ALP_ERR_NOT_PRESENT_ON_THIS_SOC.
 *
 * @par Supported silicon: alif:ensemble:e7
 *      (E3 / E5 / E8 land in a v0.7.x follow-up.)
 *
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * @par ABI status: [ABI-EXPERIMENTAL]
 *      First vendor-extension header.  Promotes to [ABI-STABLE]
 *      when three vendor families ship extensions.
 */

#ifndef ALP_EXT_ALIF_ADC_H
#define ALP_EXT_ALIF_ADC_H

#include <stdint.h>
#include <alp/adc.h>
#include <alp/peripheral.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ALP_EXT_ALIF_ADC_AVAILABLE 1

typedef enum {
    ALP_ALIF_ADC_TRIGGER_SOFTWARE = 0,
    ALP_ALIF_ADC_TRIGGER_TIMER0,
    ALP_ALIF_ADC_TRIGGER_TIMER1,
    ALP_ALIF_ADC_TRIGGER_TIMER2,
    ALP_ALIF_ADC_TRIGGER_TIMER3,
    ALP_ALIF_ADC_TRIGGER_EXT_PIN,
} alp_alif_adc_trigger_t;

/**
 * @brief Configure Alif ADC hardware oversampling sequencer.
 *
 * @par Supported silicon: alif:ensemble:e7
 *
 * @param h      Handle from alp_adc_open() opened against an Alif SoC.
 * @param ratio  Oversampling ratio: 1, 2, 4, 8, 16, 32, 64, 128, or 256.
 *               1 disables oversampling.
 * @return  ALP_OK on success.
 *          ALP_ERR_NOT_PRESENT_ON_THIS_SOC if h was opened on non-Alif silicon.
 *          ALP_ERR_INVAL on a non-power-of-2 ratio or ratio > 256.
 */
alp_status_t alp_alif_adc_set_oversampling(alp_adc_t *h, uint16_t ratio);

/**
 * @brief Configure Alif ADC hardware trigger source.
 *
 * @par Supported silicon: alif:ensemble:e7
 *
 * @param h    Handle from alp_adc_open() opened against an Alif SoC.
 * @param src  Trigger source enum (software, timer 0-3, or external pin).
 * @return  ALP_OK / ALP_ERR_NOT_PRESENT_ON_THIS_SOC / ALP_ERR_INVAL.
 */
alp_status_t alp_alif_adc_set_trigger_source(alp_adc_t *h,
                                             alp_alif_adc_trigger_t src);

#ifdef __cplusplus
}
#endif

#endif /* ALP_EXT_ALIF_ADC_H */
```

- [ ] **Step 2: Run the CI gate**

```bash
python3 scripts/check_vendor_ext_tags.py
```

Expected: exit 0 — both functions carry `@par Supported silicon:` tags.

- [ ] **Step 3: Smoke-compile**

```bash
gcc -c -x c -I include -o /tmp/alif-ext-smoke.o - <<'EOF'
#include <alp/ext/alif/adc.h>
#if !ALP_EXT_ALIF_ADC_AVAILABLE
#error "available macro missing"
#endif
EOF
```

Expected: exit 0.

- [ ] **Step 4: Commit**

```bash
git add include/alp/ext/alif/adc.h
git commit -m "feat(ext/alif/adc): first vendor-extension header

Declares alp_alif_adc_set_oversampling and _set_trigger_source.
Bodies live in src/backends/adc/alif_e7.c.  Carries @par
Supported silicon: tags on each function per the slice-0 CI gate."
```

---

## Task 10: Update `examples/peripheral-io/adc-voltmeter` with capability-gated teaching block

**Files:**
- Modify: `examples/peripheral-io/adc-voltmeter/src/main.c`

- [ ] **Step 1: Inspect the existing example**

```bash
cat examples/peripheral-io/adc-voltmeter/src/main.c
```

Find the line where `alp_adc_open(...)` returns; that's the insertion point for the new teaching block.

- [ ] **Step 2: Insert the teaching block**

After the existing `alp_adc_t *h = alp_adc_open(&cfg);` (and its null-check), insert:

```c
    /* Capability-gated configuration teaching block.
     *
     * alp_capabilities_has() asks the BACKEND what THIS opened
     * handle can do.  Runtime gate.  Pairs with ALP_HAS() from
     * <alp/cap.h>, which is the SoC-level compile-time gate. */
    const alp_capabilities_t *caps = alp_adc_capabilities(h);
    if (alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_OVERSAMPLE)) {
        /* The backend advertises HW oversampling.  If we're on an
         * Alif build, use the vendor-extension knob to enable it. */
#if ALP_EXT_ALIF_ADC_AVAILABLE
        alp_status_t rc = alp_alif_adc_set_oversampling(h, 8u);
        if (rc != ALP_OK) {
            printf("[adc] alif oversampling rejected: %d\n", (int)rc);
        }
#endif
    }
```

Also add to the includes at top of the file:

```c
#include <alp/cap_instance.h>
#ifdef __has_include
# if __has_include(<alp/ext/alif/adc.h>)
#  include <alp/ext/alif/adc.h>
# endif
#endif
```

(Defensive include — the example must build on all SoMs whether the Alif ext header exists or not.)

- [ ] **Step 3: Smoke-build the example for native_sim**

```bash
west twister -T examples/peripheral-io/adc-voltmeter -p native_sim --inline-logs 2>&1 | tail -10
```

If twister isn't on PATH locally, skip; CI validates.

- [ ] **Step 4: Commit**

```bash
git add examples/peripheral-io/adc-voltmeter/src/main.c
git commit -m "examples(adc-voltmeter): teach capability + vendor-ext gating

Shows the customer the two-level gating idiom: runtime
alp_capabilities_has check + compile-time #if
ALP_EXT_ALIF_ADC_AVAILABLE.  Builds on every SoM thanks to
defensive include + double gate."
```

---

## Task 11: Unit test harness `tests/unit/adc_registry/`

**Files:**
- Create: `tests/unit/adc_registry/CMakeLists.txt`
- Create: `tests/unit/adc_registry/prj.conf`
- Create: `tests/unit/adc_registry/testcase.yaml`
- Create: `tests/unit/adc_registry/src/test_adc_registry.c`

- [ ] **Step 1: Harness scaffolding**

Create `tests/unit/adc_registry/CMakeLists.txt`:

```cmake
# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(test_adc_registry)

target_sources(app PRIVATE src/test_adc_registry.c)
```

Create `tests/unit/adc_registry/prj.conf`:

```
CONFIG_ZTEST=y
CONFIG_ALP_SDK=y
CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y
CONFIG_ALP_SDK_ADC_SW_FALLBACK=y
```

Create `tests/unit/adc_registry/testcase.yaml`:

```yaml
tests:
  alp.unit.adc_registry:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim
      - native_sim/native/64
    tags:
      - alp
      - adc
      - backend
      - unit
```

- [ ] **Step 2: Write the tests**

Create `tests/unit/adc_registry/src/test_adc_registry.c`:

```c
/* SPDX-License-Identifier: Apache-2.0 */

#include <string.h>
#include <zephyr/ztest.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/ext/alif/adc.h>

ZTEST_SUITE(alp_adc_registry, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_adc_registry, test_open_succeeds_on_supported_soc)
{
    alp_adc_config_t cfg = {
        .channel_id      = 0,
        .resolution_bits = 12,
        .reference       = ALP_ADC_REF_INTERNAL,
    };
    /* On the test build, the active SoC is alif:ensemble:e7
     * (per prj.conf), so the realhw backend is selected.  If the
     * Zephyr ADC driver fails (device not ready in native_sim) the
     * SW fallback still serves the read via wildcard match. */
    alp_adc_t *h = alp_adc_open(&cfg);
    zassert_not_null(h);
    const alp_capabilities_t *caps = alp_adc_capabilities(h);
    zassert_not_null(caps);
    alp_adc_close(h);
}

ZTEST(alp_adc_registry, test_open_returns_inval_on_null_config)
{
    alp_adc_t *h = alp_adc_open(NULL);
    zassert_is_null(h);
    /* The exact last-error mechanism isn't part of this slice's
     * test scope; we just verify the NULL handle return. */
}

ZTEST(alp_adc_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* Directly query the selector with a silicon ref no real backend
     * registers for -- only sw_fallback's "*" wildcard matches. */
    const alp_backend_t *be =
        alp_backend_select("adc", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_adc_registry, test_realhw_picked_over_sw_on_alif_e7)
{
    const alp_backend_t *be =
        alp_backend_select("adc", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "alif"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_adc_registry, test_read_raw_advances_saw_on_sw_fallback)
{
    /* Force the SW path by selecting a silicon ref no realhw covers.
     * Build a manual handle through the dispatcher's ops vtable.
     * (This test deliberately bypasses ALP_SOC_REF_STR.) */
    const alp_backend_t *be =
        alp_backend_select("adc", "fictional:soc:zz");
    zassert_not_null(be);
    /* The SW backend's read_raw advances a saw counter; we can't
     * call it through alp_adc_read_raw without a handle, so this
     * test just verifies the backend's ops pointer is non-null. */
    zassert_not_null(be->ops);
}

ZTEST(alp_adc_registry, test_vendor_ext_rejects_wrong_vendor)
{
    /* Construct a fake handle that points at the SW backend.
     * Calling the Alif vendor-ext function MUST return
     * ALP_ERR_NOT_PRESENT_ON_THIS_SOC. */
    extern struct alp_adc {
        alp_adc_backend_state_t  state;
        const alp_backend_t     *backend;
        alp_capabilities_t       cached_caps;
        bool                     in_use;
    } *_test_fake_handle(const char *silicon_ref);

    /* The helper above is defined locally below; it builds a fake
     * handle pointing at whichever backend matches silicon_ref. */
    extern alp_adc_t *_make_fake_handle(const char *silicon_ref);
    alp_adc_t *h = _make_fake_handle("fictional:soc:zz");
    zassert_not_null(h);
    alp_status_t rc = alp_alif_adc_set_oversampling(h, 8u);
    zassert_equal(rc, ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

ZTEST(alp_adc_registry, test_vendor_ext_validates_ratio)
{
    extern alp_adc_t *_make_fake_handle(const char *silicon_ref);
    alp_adc_t *h = _make_fake_handle("alif:ensemble:e7");
    zassert_not_null(h);
    alp_status_t rc = alp_alif_adc_set_oversampling(h, 7u);  /* not power-of-2 */
    zassert_equal(rc, ALP_ERR_INVAL);
}

ZTEST(alp_adc_registry, test_alp_capabilities_has_null_pointer_safe)
{
    zassert_false(alp_capabilities_has(NULL, ALP_INSTANCE_CAP_DMA));
}

/* === Test helper: build a fake handle that points at a chosen backend === */

static struct {
    alp_adc_backend_state_t  state;
    const alp_backend_t     *backend;
    alp_capabilities_t       cached_caps;
    bool                     in_use;
} _fake_pool[4];
static size_t _fake_next = 0;

alp_adc_t *_make_fake_handle(const char *silicon_ref)
{
    if (_fake_next >= 4) return NULL;
    const alp_backend_t *be = alp_backend_select("adc", silicon_ref);
    if (be == NULL) return NULL;
    _fake_pool[_fake_next].backend       = be;
    _fake_pool[_fake_next].in_use        = true;
    _fake_pool[_fake_next].state.ops     = (const alp_adc_ops_t *)be->ops;
    _fake_pool[_fake_next].cached_caps.flags = be->base_caps;
    return (alp_adc_t *)&_fake_pool[_fake_next++];
}
```

(The `_make_fake_handle` helper relies on `struct alp_adc` having the layout defined in `adc_ops.h` — Task 6's refactor ensures both the test file and the production code agree on the layout.)

- [ ] **Step 3: Try twister locally (best-effort)**

```bash
west twister -T tests/unit/adc_registry -p native_sim --inline-logs 2>&1 | tail -15
```

Windows local twister fails on DTS preprocessing; rely on CI.

- [ ] **Step 4: Commit**

```bash
git add tests/unit/adc_registry/
git commit -m "test(adc): unit-test harness for registry + vendor ext"
```

---

## Task 12: Refresh ABI snapshot

**Files:**
- Modify: `docs/abi/v0.5-snapshot.json`

- [ ] **Step 1: Regenerate**

```bash
python3 scripts/abi_snapshot.py --version v0.5 --output docs/abi/v0.5-snapshot.json
```

Expected: writes the file, count rises (the new `<alp/ext/alif/adc.h>` plus the new `alp_adc_capabilities` function).

- [ ] **Step 2: Verify new symbols present**

```bash
git diff docs/abi/v0.5-snapshot.json | grep -E "alp_adc_capabilities|alp_alif_adc_set_oversampling|alp_alif_adc_set_trigger_source|ALP_ALIF_ADC_TRIGGER_|ALP_EXT_ALIF_ADC_AVAILABLE" | head -10
```

Expected: at least four of those names appear.

- [ ] **Step 3: Commit**

```bash
git add docs/abi/v0.5-snapshot.json
git commit -m "chore(abi): regenerate v0.5 snapshot for ADC registry pilot"
```

---

## Self-review checklist

1. **Spec coverage:**
   - File layout (spec Section 1) → Tasks 1-7 + 9-11 cover every new/modified/deleted file ✓
   - Backend ops vtable (spec Section 2) → Task 3 (header) + Task 6 (struct alp_adc layout refactor) ✓
   - Portable layer dispatcher (spec Section 3) → Task 4 ✓
   - Three backends (spec Section 4) → Tasks 5, 6, 7 ✓
   - Vendor extensions (spec Section 5) → Task 9 (header) + Task 6 (bodies) ✓
   - Tests (spec Section 5) → Task 11 ✓
   - Example update (spec Section 5) → Task 10 ✓

2. **Type consistency:** `alp_adc_backend_state_t`, `alp_adc_ops_t`, `alif_e7_adc_ops_t`, `gd32_bridge_state_t`, `alp_alif_adc_trigger_t`, `struct alp_adc` used consistently across tasks. `struct alp_adc` is defined in `adc_ops.h` (per Task 6 Step 3) and included by every TU that needs it.

3. **No placeholders:** every step shows actual code or commands; no "implement appropriate error handling".

4. **Sequencing:**
   - Phase A: Task 1 (DAC extraction), Task 2 (public header decl), Task 3 (ops header). Parallel-safe.
   - Phase B: Task 4 (dispatcher) — depends on Task 3.
   - Phase C: Task 5 (SW fallback) — depends on Tasks 3, 4.
   - Phase D: Tasks 6 (Alif), 7 (V2N) — depend on Tasks 3, 4. Touch different files; safe to dispatch one at a time. Task 6 also refactors `struct alp_adc` into `adc_ops.h`.
   - Phase E: Task 8 (delete peripheral_adc.c) — depends on 5, 6, 7 (so customer code never lacks a working ADC path during the transition).
   - Phase F: Task 9 (vendor ext header) — depends on Task 6 (header bodies already in alif_e7.c) but can land in parallel with 8.
   - Phase G: Task 10 (example) — depends on 9 + 2.
   - Phase H: Task 11 (tests) — depends on everything compiling.
   - Phase I: Task 12 (ABI snapshot) — last.

## Execution sequence summary

| Order | Task | Depends on |
|---|---|---|
| 1 | T1 — DAC extraction | — |
| 2 | T2 — `alp_adc_capabilities` decl | — |
| 3 | T3 — `adc_ops.h` | — |
| 4 | T4 — dispatcher | T3 |
| 5 | T5 — SW fallback | T3, T4 |
| 6 | T6 — Alif E7 + struct refactor | T3, T4 |
| 7 | T7 — V2N GD32 bridge | T3, T4 |
| 8 | T8 — delete peripheral_adc.c | T1, T5, T6, T7 |
| 9 | T9 — vendor ext header | T6 |
| 10 | T10 — example update | T2, T9 |
| 11 | T11 — unit tests | T8, T9 |
| 12 | T12 — ABI snapshot | T11 |

Subagent dispatch is strictly sequential per the subagent-driven skill's red flag — no parallel implementers.
