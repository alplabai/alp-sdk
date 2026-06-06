# mproc shmem + hwsem implementation (Track 4 M1)

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace the `ALP_ERR_NOSUPPORT` stubs in `alp_shmem_*` and `alp_hwsem_*` (in `src/zephyr/mproc_zephyr.c`) with real implementations, gated by Zephyr DT aliases (`alp-shmemN`) for shmem and k_sem fallback for hwsem.  Closes the assessment-surfaced gap that `<alp/mproc.h>`'s shmem + hwsem entry points were dead code.

**Architecture:** shmem resolves the `cfg->name` string against a static lookup table built from `DT_ALIAS(alp_shmemN)` nodes — Same pattern as `alp_mbox_devs[]` already in `mproc_zephyr.c`.  Each region's `base + size` come from the DT node's `reg` property.  hwsem uses an internal array of `struct k_sem` indexed by `hwsem_id` — this is an **intra-core** mutex semantic, not real cross-core HWSEM hardware (that hookup lands per-SoC in a later track when real silicon is on the bench).  The header docstring is updated to make this explicit so customers don't assume cross-core safety.

**Tech Stack:** Zephyr 4.4 (DT macros, k_sem, ZTEST), `src/zephyr/mproc_zephyr.c` (existing scaffolding), `tests/zephyr/mproc/` (existing test infra), `boards/native_sim_native_64.overlay` (new DT overlay for the test build).

---

## File structure

| File | Action | Responsibility |
|---|---|---|
| `include/alp/mproc.h` | Modify | Update `alp_shmem_*` + `alp_hwsem_*` docstrings to reflect new semantics (k_sem intra-core fallback for hwsem; DT-aliased regions for shmem). |
| `src/zephyr/mproc_zephyr.c` | Modify | Replace shmem `NOSUPPORT` stub with DT-aliased lookup; replace hwsem stub with `k_sem` array. |
| `tests/zephyr/mproc/src/main.c` | Modify | Add new tests for shmem name-resolution + hwsem lifecycle.  Move existing "no backend" tests under a clearer `#if !defined(CONFIG_ALP_SDK_MPROC)` guard so the new MPROC=y scenario doesn't run them. |
| `tests/zephyr/mproc/boards/native_sim_native_64.overlay` | Create | DT overlay declaring `alp-shmem0` + `alp-shmem1` aliases + matching `mmio-sram` nodes. |
| `tests/zephyr/mproc/prj_shmem_hwsem.conf` | Create | Conf file overlay turning on `CONFIG_ALP_SDK_MPROC=y` for the new scenario. |
| `tests/zephyr/mproc/testcase.yaml` | Modify | Add scenario `alp_sdk.mproc.shmem_hwsem` that pulls in the overlay + prj_shmem_hwsem.conf. |

---

## Task 1: shmem DT-aliased lookup + impl

**Files:**
- Create: `tests/zephyr/mproc/boards/native_sim_native_64.overlay`
- Create: `tests/zephyr/mproc/prj_shmem_hwsem.conf`
- Modify: `tests/zephyr/mproc/src/main.c` (add tests; guard existing tests)
- Modify: `tests/zephyr/mproc/testcase.yaml` (add scenario)
- Modify: `src/zephyr/mproc_zephyr.c` (lines 164-209 — shmem block)

- [ ] **Step 1: Write the failing shmem tests**

Append at the bottom of `tests/zephyr/mproc/src/main.c` (before any `#endif` at file end):

```c
/* ------------------------------------------------------------------ */
/* Real backend (CONFIG_ALP_SDK_MPROC=y + DT overlay supplying        */
/* alp-shmem0..1 + the k_sem-backed hwsem fallback).                  */
/* ------------------------------------------------------------------ */
#if defined(CONFIG_ALP_SDK_MPROC) && DT_HAS_ALIAS(alp_shmem0)

ZTEST(alp_mproc, test_shmem_open_resolves_name)
{
    alp_shmem_config_t cfg = {.name = "alp_shmem0", .size = 0, .cacheable = false};
    alp_shmem_t       *s   = alp_shmem_open(&cfg);
    zassert_not_null(s, "open should resolve alp_shmem0 alias");

    void  *base = NULL;
    size_t size = 0;
    zassert_equal(alp_shmem_view(s, &base, &size), ALP_OK);
    zassert_equal((uintptr_t)base, (uintptr_t)DT_REG_ADDR(DT_ALIAS(alp_shmem0)),
                  "base must match DT reg-address");
    zassert_equal(size, DT_REG_SIZE(DT_ALIAS(alp_shmem0)),
                  "size must match DT reg-size");

    alp_shmem_close(s);
}

ZTEST(alp_mproc, test_shmem_open_unknown_name_returns_null)
{
    alp_shmem_config_t cfg = {.name = "nope_not_a_region",
                              .size = 0, .cacheable = false};
    zassert_is_null(alp_shmem_open(&cfg));
    zassert_equal(alp_last_error(), ALP_ERR_NOT_READY);
}

ZTEST(alp_mproc, test_shmem_open_two_regions)
{
    alp_shmem_config_t cfg0 = {.name = "alp_shmem0", .size = 0};
    alp_shmem_config_t cfg1 = {.name = "alp_shmem1", .size = 0};
    alp_shmem_t       *s0   = alp_shmem_open(&cfg0);
    alp_shmem_t       *s1   = alp_shmem_open(&cfg1);
    zassert_not_null(s0);
    zassert_not_null(s1);
    zassert_not_equal((void *)s0, (void *)s1, "distinct handles for distinct regions");

    void  *base0 = NULL, *base1 = NULL;
    size_t size0 = 0,    size1 = 0;
    zassert_equal(alp_shmem_view(s0, &base0, &size0), ALP_OK);
    zassert_equal(alp_shmem_view(s1, &base1, &size1), ALP_OK);
    zassert_not_equal((uintptr_t)base0, (uintptr_t)base1);

    alp_shmem_close(s0);
    alp_shmem_close(s1);
}

ZTEST(alp_mproc, test_shmem_open_exhausts_pool)
{
    /* CONFIG_ALP_SDK_MAX_SHMEM_HANDLES = 2 by default; open both then
     * verify a third open with the same name fails with NOMEM. */
    alp_shmem_config_t cfg = {.name = "alp_shmem0", .size = 0};
    alp_shmem_t *a = alp_shmem_open(&cfg);
    alp_shmem_t *b = alp_shmem_open(&cfg);
    zassert_not_null(a);
    zassert_not_null(b);
    zassert_is_null(alp_shmem_open(&cfg));
    zassert_equal(alp_last_error(), ALP_ERR_NOMEM);
    alp_shmem_close(a);
    alp_shmem_close(b);
}

#endif /* CONFIG_ALP_SDK_MPROC && DT_HAS_ALIAS(alp_shmem0) */
```

- [ ] **Step 2: Create the DT overlay for the new scenario**

Create `tests/zephyr/mproc/boards/native_sim_native_64.overlay`:

```dts
/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * DT overlay for the alp_sdk.mproc.shmem_hwsem ZTEST scenario.
 * Declares two alp-shmem aliases backed by mmio-sram nodes with
 * arbitrary "addresses" -- native_sim never dereferences them; the
 * tests only verify the wrapper reports the DT-anchored base+size
 * verbatim.  Real-silicon boards define the same aliases against
 * actual reserved-memory carve-outs (orchestrator-allocated).
 */

/ {
    alp_shmem0_node: shmem@70000000 {
        compatible = "mmio-sram";
        reg = <0x70000000 0x1000>;
        status = "okay";
    };

    alp_shmem1_node: shmem@70001000 {
        compatible = "mmio-sram";
        reg = <0x70001000 0x800>;
        status = "okay";
    };

    aliases {
        alp-shmem0 = &alp_shmem0_node;
        alp-shmem1 = &alp_shmem1_node;
    };
};
```

- [ ] **Step 3: Create the prj.conf overlay for the new scenario**

Create `tests/zephyr/mproc/prj_shmem_hwsem.conf`:

```
# SPDX-License-Identifier: Apache-2.0
#
# Extra config for the alp_sdk.mproc.shmem_hwsem scenario: turn the
# real MPROC backend on so alp_shmem_* + alp_hwsem_* dispatch
# through the DT-alias-lookup + k_sem paths (rather than the
# NOSUPPORT shortcircuit).

CONFIG_ALP_SDK_MPROC=y
```

- [ ] **Step 4: Add the new testcase.yaml scenario**

Edit `tests/zephyr/mproc/testcase.yaml`, append below the existing `alp_sdk.mproc.nanopb_framing` block:

```yaml

  # 2026-05-17: real-backend exercise for shmem + hwsem -- pulls in
  # the native_sim DT overlay declaring alp-shmem0/1 + flips
  # CONFIG_ALP_SDK_MPROC=y so the wrapper takes the DT-alias-lookup
  # path (shmem) + the k_sem fallback (hwsem) instead of the
  # NOSUPPORT shortcircuit.
  alp_sdk.mproc.shmem_hwsem:
    platform_allow:
      - native_sim
      - native_sim/native/64
    integration_platforms:
      - native_sim/native/64
    extra_args:
      - "EXTRA_CONF_FILE=prj_shmem_hwsem.conf"
    tags:
      - alp-sdk
      - mproc
```

- [ ] **Step 5: Run the new tests to verify they fail**

Run (from repo root, with `ZEPHYR_BASE` set):

```bash
python3 $ZEPHYR_BASE/scripts/twister --testsuite-root tests/zephyr/mproc \
    -p native_sim/native/64 \
    --scenario alp_sdk.mproc.shmem_hwsem --inline-logs
```

Expected: `test_shmem_open_resolves_name` FAILs with `s` being NULL — the impl still shortcircuits to `ALP_ERR_NOSUPPORT`.

- [ ] **Step 6: Implement the shmem path**

Edit `src/zephyr/mproc_zephyr.c`.  Replace the `Shared memory` section (lines ~164-209) with:

```c
/* ================================================================== */
/* Shared memory                                                       */
/* ================================================================== */

#if defined(CONFIG_ALP_SDK_MPROC)

#include <zephyr/devicetree.h>
#include <zephyr/sys/util_macro.h>

struct alp_shmem_region {
    const char *name;       /* matches alp_shmem_open's cfg->name */
    void       *base;
    size_t      size;
};

/* Build a const lookup table from DT_ALIAS(alp_shmemN).  IF_ENABLED
 * skips the entry when the alias isn't defined.  Up to 4 regions
 * supported -- raise the upper bound here if a SoM needs more. */
#define ALP_SHMEM_REGION_ENTRY_IF(idx)                                 \
    IF_ENABLED(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_shmem, idx))),       \
               ({ .name = "alp_shmem" #idx,                            \
                  .base = (void *)DT_REG_ADDR(                          \
                              DT_ALIAS(_CONCAT(alp_shmem, idx))),       \
                  .size = (size_t)DT_REG_SIZE(                          \
                              DT_ALIAS(_CONCAT(alp_shmem, idx))),       \
               },))

static const struct alp_shmem_region alp_shmem_regions[] = {
    ALP_SHMEM_REGION_ENTRY_IF(0)
    ALP_SHMEM_REGION_ENTRY_IF(1)
    ALP_SHMEM_REGION_ENTRY_IF(2)
    ALP_SHMEM_REGION_ENTRY_IF(3)
};
#define ALP_SHMEM_REGION_COUNT \
    (sizeof(alp_shmem_regions) / sizeof(alp_shmem_regions[0]))

#endif /* CONFIG_ALP_SDK_MPROC */

alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg)
{
    alp_z_clear_last_error();
    if (cfg == NULL || cfg->name == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
#if defined(CONFIG_ALP_SDK_MPROC)
    /* Resolve cfg->name against the DT-alias lookup table. */
    const struct alp_shmem_region *region = NULL;
    for (size_t i = 0; i < ALP_SHMEM_REGION_COUNT; ++i) {
        if (strcmp(alp_shmem_regions[i].name, cfg->name) == 0) {
            region = &alp_shmem_regions[i];
            break;
        }
    }
    if (region == NULL) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }
    struct alp_shmem *s = shmem_pool_acquire();
    if (s == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    s->base = region->base;
    s->size = region->size;
    return s;
#else
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}
```

The existing `alp_shmem_view` + `alp_shmem_close` already return the right thing once `s->base` + `s->size` are populated — no change needed there.

- [ ] **Step 7: Run the shmem tests, verify they pass**

```bash
python3 $ZEPHYR_BASE/scripts/twister --testsuite-root tests/zephyr/mproc \
    -p native_sim/native/64 \
    --scenario alp_sdk.mproc.shmem_hwsem --inline-logs
```

Expected: All 4 new shmem tests PASS.  Existing tests (the no-backend `test_shmem_open_no_backend_returns_null` etc.) are skipped under the new scenario because of the `#if !defined(CONFIG_ALP_SDK_MPROC)` guard at line 28 of `main.c`.

- [ ] **Step 8: Commit**

```bash
git add src/zephyr/mproc_zephyr.c \
        tests/zephyr/mproc/src/main.c \
        tests/zephyr/mproc/testcase.yaml \
        tests/zephyr/mproc/prj_shmem_hwsem.conf \
        tests/zephyr/mproc/boards/native_sim_native_64.overlay
git commit -m "mproc: DT-anchored shmem impl (alp_shmem_open resolves by name)

Replaces the ALP_ERR_NOSUPPORT stub in alp_shmem_open with a
DT-alias lookup: cfg->name is matched against alp_shmemN aliases
declared in the board's DT (or a per-test overlay).  alp_shmem_view
returns the DT node's reg-address + reg-size verbatim.

Up to 4 regions supported in the lookup table; raise the upper
bound by adding ALP_SHMEM_REGION_ENTRY_IF(N) lines.  Real-silicon
boards declare the aliases against actual reserved-memory carve-
outs (the v0.6 orchestrator allocates them).  Native_sim test
overlay uses mmio-sram nodes with arbitrary addresses -- the
tests verify the wrapper returns those values verbatim; the
addresses themselves are never dereferenced.

New ZTESTs under alp_sdk.mproc.shmem_hwsem scenario (MPROC=y +
the new overlay) cover: name resolves to handle, unknown name
returns NULL+NOT_READY, two distinct regions yield distinct
handles, pool exhaustion returns NOMEM.  Existing no-backend
ZTESTs stay under their MPROC=n guard."
```

---

## Task 2: hwsem k_sem-backed impl

**Files:**
- Modify: `src/zephyr/mproc_zephyr.c` (lines ~370-425 — hwsem block)
- Modify: `tests/zephyr/mproc/src/main.c` (add hwsem tests)

- [ ] **Step 1: Write the failing hwsem tests**

Append at the bottom of `tests/zephyr/mproc/src/main.c` (inside the same `#if defined(CONFIG_ALP_SDK_MPROC) && DT_HAS_ALIAS(alp_shmem0)` block as the shmem tests — the hwsem fallback uses no DT itself, but co-locating under the MPROC=y guard is correct):

```c
ZTEST(alp_mproc, test_hwsem_lock_unlock_cycle)
{
    alp_hwsem_t *sem = alp_hwsem_open(0);
    zassert_not_null(sem);

    zassert_equal(alp_hwsem_try_lock(sem), ALP_OK);
    /* Holding -- a second try_lock from the SAME handle re-locks
     * (k_sem is counting; the fallback's contract is sticky locks
     * across distinct handles to the same id, see below). */

    zassert_equal(alp_hwsem_unlock(sem), ALP_OK);
    alp_hwsem_close(sem);
}

ZTEST(alp_mproc, test_hwsem_distinct_handles_same_id_contend)
{
    alp_hwsem_t *a = alp_hwsem_open(1);
    alp_hwsem_t *b = alp_hwsem_open(1);
    zassert_not_null(a);
    zassert_not_null(b);

    zassert_equal(alp_hwsem_try_lock(a), ALP_OK);
    /* Different handle, same hwsem_id -> second try_lock must fail. */
    zassert_equal(alp_hwsem_try_lock(b), ALP_ERR_BUSY);
    zassert_equal(alp_hwsem_unlock(a), ALP_OK);
    /* After release the second handle can take it. */
    zassert_equal(alp_hwsem_try_lock(b), ALP_OK);
    zassert_equal(alp_hwsem_unlock(b), ALP_OK);

    alp_hwsem_close(a);
    alp_hwsem_close(b);
}

ZTEST(alp_mproc, test_hwsem_lock_with_timeout_times_out)
{
    alp_hwsem_t *a = alp_hwsem_open(2);
    alp_hwsem_t *b = alp_hwsem_open(2);
    zassert_equal(alp_hwsem_try_lock(a), ALP_OK);
    /* Blocking lock on the same id must time out within ~50 ms. */
    zassert_equal(alp_hwsem_lock(b, 50), ALP_ERR_TIMEOUT);
    zassert_equal(alp_hwsem_unlock(a), ALP_OK);
    alp_hwsem_close(a);
    alp_hwsem_close(b);
}

ZTEST(alp_mproc, test_hwsem_open_out_of_range)
{
    /* The fallback supports hwsem_id < 16. */
    zassert_is_null(alp_hwsem_open(99));
    zassert_equal(alp_last_error(), ALP_ERR_OUT_OF_RANGE);
}

ZTEST(alp_mproc, test_hwsem_unlock_without_lock_returns_inval)
{
    alp_hwsem_t *sem = alp_hwsem_open(3);
    zassert_not_null(sem);
    /* Not held -- unlock must reject so a buggy app doesn't release
     * a hwsem it never took. */
    zassert_equal(alp_hwsem_unlock(sem), ALP_ERR_INVAL);
    alp_hwsem_close(sem);
}
```

- [ ] **Step 2: Run tests to verify they fail**

```bash
python3 $ZEPHYR_BASE/scripts/twister --testsuite-root tests/zephyr/mproc \
    -p native_sim/native/64 \
    --scenario alp_sdk.mproc.shmem_hwsem --inline-logs
```

Expected: All 5 new hwsem tests FAIL — current impl returns `ALP_ERR_NOSUPPORT` from `alp_hwsem_try_lock` / `_lock` / `_unlock`.

- [ ] **Step 3: Implement the hwsem path with k_sem fallback**

Edit `src/zephyr/mproc_zephyr.c`.  Replace the `Hardware semaphore` section (lines ~366-425) with:

```c
/* ================================================================== */
/* Hardware semaphore                                                  */
/* ================================================================== */

/* Intra-core fallback: an array of k_sem indexed by hwsem_id, with
 * count=1 (mutex semantics).  This serializes access WITHIN one
 * Zephyr image but does NOT cross cores -- a peer Zephyr / Linux
 * image on the same SoM uses a DIFFERENT k_sem array and won't see
 * the lock.  Per-SoC real-HWSEM (AEN HWSEM block, ST HSEM, etc.)
 * land per-SoC in a follow-on track; until then call sites that
 * need cross-core mutex must use a real shared-state primitive
 * (DT-anchored memory + atomic ops) instead.
 *
 * hwsem_id range: 0..15.  Bump CONFIG_ALP_SDK_MPROC_HWSEM_COUNT
 * (added in zephyr/Kconfig) if a SoM needs more. */

#ifndef CONFIG_ALP_SDK_MPROC_HWSEM_COUNT
#define CONFIG_ALP_SDK_MPROC_HWSEM_COUNT 16
#endif

#if defined(CONFIG_ALP_SDK_MPROC)
static struct k_sem alp_hwsem_kobjs[CONFIG_ALP_SDK_MPROC_HWSEM_COUNT];
static bool         alp_hwsem_kobjs_initialised;
static struct k_spinlock alp_hwsem_init_lock;

static void hwsem_kobjs_init_once(void)
{
    k_spinlock_key_t key = k_spin_lock(&alp_hwsem_init_lock);
    if (!alp_hwsem_kobjs_initialised) {
        for (size_t i = 0; i < CONFIG_ALP_SDK_MPROC_HWSEM_COUNT; ++i) {
            k_sem_init(&alp_hwsem_kobjs[i], 1, 1);
        }
        alp_hwsem_kobjs_initialised = true;
    }
    k_spin_unlock(&alp_hwsem_init_lock, key);
}
#endif

alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id)
{
    alp_z_clear_last_error();
#if defined(CONFIG_ALP_SDK_MPROC)
    if (hwsem_id >= CONFIG_ALP_SDK_MPROC_HWSEM_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }
    hwsem_kobjs_init_once();
    struct alp_hwsem *s = hwsem_pool_acquire();
    if (s == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    s->hwsem_id = hwsem_id;
    s->held     = false;
    return s;
#else
    (void)hwsem_id;
    alp_z_set_last_error(ALP_ERR_NOSUPPORT);
    return NULL;
#endif
}

alp_status_t alp_hwsem_try_lock(alp_hwsem_t *sem)
{
    if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_MPROC)
    /* k_sem_take with K_NO_WAIT returns -EBUSY when the count is 0. */
    int rc = k_sem_take(&alp_hwsem_kobjs[sem->hwsem_id], K_NO_WAIT);
    if (rc == -EBUSY) return ALP_ERR_BUSY;
    if (rc != 0)      return errno_to_alp(rc);
    sem->held = true;
    return ALP_OK;
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_hwsem_lock(alp_hwsem_t *sem, uint32_t timeout_ms)
{
    if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_MPROC)
    k_timeout_t to = (timeout_ms == UINT32_MAX)
        ? K_FOREVER : K_MSEC(timeout_ms);
    int rc = k_sem_take(&alp_hwsem_kobjs[sem->hwsem_id], to);
    if (rc == -EAGAIN) return ALP_ERR_TIMEOUT;
    if (rc != 0)       return errno_to_alp(rc);
    sem->held = true;
    return ALP_OK;
#else
    (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
#endif
}

alp_status_t alp_hwsem_unlock(alp_hwsem_t *sem)
{
    if (sem == NULL || !sem->in_use) return ALP_ERR_NOT_READY;
#if defined(CONFIG_ALP_SDK_MPROC)
    if (!sem->held) return ALP_ERR_INVAL;
    k_sem_give(&alp_hwsem_kobjs[sem->hwsem_id]);
    sem->held = false;
    return ALP_OK;
#else
    return ALP_ERR_NOSUPPORT;
#endif
}

void alp_hwsem_close(alp_hwsem_t *sem)
{
    if (sem == NULL || !sem->in_use) return;
#if defined(CONFIG_ALP_SDK_MPROC)
    /* If still held when closed, give the kobj back so the next opener
     * can take it -- not the caller's bug to leak the lock. */
    if (sem->held) {
        k_sem_give(&alp_hwsem_kobjs[sem->hwsem_id]);
        sem->held = false;
    }
    sem->in_use = false;
#endif
}
```

- [ ] **Step 4: Run hwsem tests, verify they pass**

```bash
python3 $ZEPHYR_BASE/scripts/twister --testsuite-root tests/zephyr/mproc \
    -p native_sim/native/64 \
    --scenario alp_sdk.mproc.shmem_hwsem --inline-logs
```

Expected: All 5 new hwsem tests PASS.  All 4 shmem tests from Task 1 still PASS.

- [ ] **Step 5: Commit**

```bash
git add src/zephyr/mproc_zephyr.c tests/zephyr/mproc/src/main.c
git commit -m "mproc: k_sem-backed hwsem fallback impl

Replaces the ALP_ERR_NOSUPPORT stubs in alp_hwsem_try_lock /
alp_hwsem_lock / alp_hwsem_unlock with a k_sem-backed fallback:
an array of CONFIG_ALP_SDK_MPROC_HWSEM_COUNT (default 16)
k_sems, indexed by hwsem_id, count=1 each (mutex semantics).
Lazy-initialised on first alp_hwsem_open() via a spinlock-
guarded once-flag.

This serializes access WITHIN one Zephyr image but does NOT
cross cores -- a peer Zephyr / Linux image on the same SoM has
a different k_sem array and won't see the lock.  Per-SoC real
HWSEM (AEN HWSEM block, ST HSEM, etc.) hookups are tracked
separately under Track 1 HiL bring-up; until then call sites
needing cross-core mutex must use DT-anchored shared memory +
atomic ops instead.

Five new ZTESTs cover: lock+unlock cycle, distinct handles to
same id contend, blocking lock times out as configured, open
beyond hwsem_id limit returns OUT_OF_RANGE, unlock-without-lock
returns INVAL (catches buggy callers).

The mproc.h docstring is updated separately in a follow-up
commit to call out the intra-core semantics for customer
visibility."
```

---

## Task 3: Update mproc.h docstring + full-suite verification

**Files:**
- Modify: `include/alp/mproc.h` (docstring updates for shmem + hwsem)

- [ ] **Step 1: Update the `alp_shmem_*` docstring section**

Edit `include/alp/mproc.h`.  Find the `Shared memory` block (around lines 60-103) and update the `alp_shmem_open` docstring to reflect the new DT-alias semantics:

```c
/**
 * @brief Acquire access to a named shared-memory region.
 *
 * Resolves @c cfg->name against the build's DT aliases
 * (`alp-shmem0`, `alp-shmem1`, ...).  Each alias points at a DT
 * node whose `reg` property carries the region's base address +
 * size.  Both cores opening the same alias-name see the same
 * physical bytes; cache coherency is the caller's responsibility
 * unless @c cfg->cacheable == false.
 *
 * @note On Zephyr-on-AEN the loader emits alp-shmem aliases for
 *       each `ipc.kind: raw_shmem` carve-out declared in
 *       board.yaml.  Custom boards can also declare aliases by
 *       hand via a DTS overlay.  Under native_sim the test build
 *       provides synthetic aliases for API-contract testing; the
 *       returned base pointers are never dereferenced because no
 *       peer process is on the other side.
 *
 * @param[in] cfg  Configuration.  Must be non-NULL with a non-empty name.
 * @return Open handle on success, or NULL if the alias isn't
 *         declared (`ALP_ERR_NOT_READY`), the handle pool is full
 *         (`ALP_ERR_NOMEM`), or @c cfg / @c cfg->name is NULL
 *         (`ALP_ERR_INVAL`).
 */
alp_shmem_t *alp_shmem_open(const alp_shmem_config_t *cfg);
```

- [ ] **Step 2: Update the `alp_hwsem_*` section header docstring**

Find the `Hardware semaphore` section (around lines 164-210) and replace the section header + the `alp_hwsem_open` docstring:

```c
/* ------------------------------------------------------------------ */
/* Hardware semaphore                                                  */
/*                                                                     */
/* @note v0.7 ships a k_sem-backed FALLBACK -- the array serializes   */
/* access WITHIN one Zephyr image but does NOT cross cores.  A peer   */
/* Zephyr / Linux image on the same SoM has its own k_sem array and  */
/* won't see the lock.  Per-SoC real HWSEM hardware (AEN HWSEM, ST   */
/* HSEM, ...) hookups land per-SoC under Track 1 HiL bring-up.  Use  */
/* DT-anchored shared memory + atomic ops for cross-core mutex until */
/* the per-SoC hookup ships on your silicon.                          */
/* ------------------------------------------------------------------ */

/** Opaque hardware-semaphore handle.  Allocate via @ref alp_hwsem_open. */
typedef struct alp_hwsem alp_hwsem_t;

/**
 * @brief Acquire a handle for a SoC hardware semaphore.
 *
 * Until per-SoC real-HWSEM backends land, this returns a handle to
 * one of @c CONFIG_ALP_SDK_MPROC_HWSEM_COUNT (default 16) k_sem-
 * backed slots indexed by @p hwsem_id.  Intra-core mutex semantics
 * only -- see the section banner above for the cross-core caveat.
 *
 * @param[in] hwsem_id  Slot index in [0,
 *                      `CONFIG_ALP_SDK_MPROC_HWSEM_COUNT`).
 * @return Open handle on success, or NULL with
 *         @ref ALP_ERR_OUT_OF_RANGE when @p hwsem_id is too big
 *         or @ref ALP_ERR_NOMEM when the handle pool is full.
 */
alp_hwsem_t *alp_hwsem_open(uint32_t hwsem_id);
```

- [ ] **Step 3: Run the full mproc test suite (all 3 scenarios)**

```bash
python3 $ZEPHYR_BASE/scripts/twister --testsuite-root tests/zephyr/mproc \
    -p native_sim/native/64 --inline-logs
```

Expected: All 3 scenarios PASS — `alp_sdk.mproc.smoke` (the existing MPROC=n no-backend tests), `alp_sdk.mproc.nanopb_framing` (the existing framing tests), `alp_sdk.mproc.shmem_hwsem` (the new MPROC=y real-backend tests).

- [ ] **Step 4: Run the full pytest sweep to confirm no regression**

```bash
python -m pytest tests/scripts/ -q
```

Expected: `286 passed, 5 skipped` (unchanged from baseline; no Python touched).

- [ ] **Step 5: Run the doc-yaml linter to confirm no doc drift**

```bash
python scripts/lint_doc_yaml_fragments.py
```

Expected: `9 fragment(s) clean`.

- [ ] **Step 6: Commit**

```bash
git add include/alp/mproc.h
git commit -m "mproc: docstring update for shmem DT-alias + hwsem fallback

Now that the impl backs both surfaces (commits prior in this
series), update the public mproc.h docstrings to tell customers
exactly what the runtime contract is:

* alp_shmem_open: resolves cfg->name against alp-shmemN DT
  aliases (board file or DTS overlay).  Returns the DT node's
  reg-address + reg-size verbatim.  ALP_ERR_NOT_READY when no
  matching alias.

* alp_hwsem_*: explicit @note in the section banner that the
  k_sem-backed fallback is INTRA-CORE ONLY.  Cross-core mutex
  callers should use DT-anchored shared memory + atomic ops
  until per-SoC real-HWSEM hookups land (tracked under Track 1
  HiL bring-up).  Per-SoC hookups will replace the k_sem
  fallback transparently when they land -- the API stays the
  same."
```

---

## Self-review

1. **Spec coverage:** Track 4 M1 deliverables — `alp_shmem_*` impl ✅ (Task 1), `alp_hwsem_*` impl ✅ (Task 2), tests added ✅ (Tasks 1 + 2), HiL coverage deferred per the roadmap.  Header docstring update ✅ (Task 3).

2. **Placeholder scan:** No `TBD` / `TODO` / "fill in later" in the steps.  All code blocks are complete + ready to paste.

3. **Type consistency:** `struct alp_shmem_region` declared in Task 1 step 6, referenced consistently as `alp_shmem_regions[]`.  `alp_hwsem_kobjs[]`, `hwsem_kobjs_init_once`, `alp_hwsem_init_lock` consistent across Task 2 step 3.  All Kconfig identifiers (`CONFIG_ALP_SDK_MPROC`, `CONFIG_ALP_SDK_MPROC_HWSEM_COUNT`) consistent.

4. **Ambiguity check:** "Per-SoC real HWSEM hookups" — explicitly noted as out-of-scope for this plan; lives in Track 1 HiL bring-up.  "Cross-core safety" — explicitly disclaimed in the hwsem docstring + commit message.
