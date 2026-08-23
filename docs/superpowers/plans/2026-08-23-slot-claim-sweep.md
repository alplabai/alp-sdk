# Slot-Claim Sweep + Regression Gate — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue: #1630** (`bug`, `area:drivers`, `area:npu`, `area:portability`, milestone `v0.17.0`)

**Goal:** Convert the nine backend pools still claiming a slot with an unlocked check-then-set onto `alp_slot_try_claim()`, then add a `scripts/check_*.py` gate so a tenth cannot be written.

**Architecture:** The sweep is the precondition; **the gate is the deliverable.** This exact class has now survived two closed issues (#1115 and #629) because both were remediated from a hand-written site list rather than from a grep. Converting nine more sites by hand and stopping would reset the clock, not close the class. Task 1 converts the eight array-shaped pools (all one mechanical shape), Task 2 converts the one singleton that an array-shaped grep structurally cannot see, Task 3 lands the gate that makes "did we get them all" a CI answer instead of a judgement call.

**Tech Stack:** C (clang-format 22.x, tabs), Zephyr, C++ for the TFLM site, Python 3.10+ stdlib for the gate, pytest for its test.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections first.

## Global Constraints

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 280 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` green before `gh pr create`.
- clang-format **22.x** on every changed `.c`/`.h`/`.cpp`.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result — a new `scripts/check_*.py` **will** drift `metadata/catalog.json`, and `test_gen_catalog` reddens both python-smoke and generated-files if you skip it.
- No AI attribution anywhere.

---

> **IMPLEMENTED 2026-08-23 — branch `fix/1630-slot-claim-atomic-sweep`, one PR
> carrying all three tasks. Read this block before the steps: the task ORDER and
> the gate's DETECTION RULE both changed, and one hole the plan did not see was
> closed.**
>
> **1. Tasks were done in the order 3 -> 1 -> 2, and that ordering is the point.**
> The plan writes the sweep first and the gate last, which makes the gate green
> from birth — never once demonstrated to detect anything. Written gate-first,
> its own `test_real_tree_is_clean` **failed with exactly the nine real sites**,
> which is the only evidence that the gate detects the defect it exists to
> detect. After the sweep it passes. Recommend keeping this order for any future
> gate in this campaign.
>
> **2. The gate matches the SET half, not the plan's test half.** The plan's
> `_TEST` regex (`if\s*\(\s*!\s*...in_use...`) plus a 4-line window cannot see
> `src/backends/inference/tflm.cpp` — that site guards a bare singleton with
> `if (g_default_arena_in_use) { fail; }`, a **positive** test with no loop and
> no subscript. That is the exact shape the plan's own Task 2 says every
> array-shaped grep walked past, so shipping an array-shaped-negation gate would
> have re-created the blind spot in CI.
>
> The shipped rule is one regex on any plain assignment of an `*in_use*` flag to
> `true`. It is complete on its own, because `alp_slot_try_claim()` never
> assigns the flag: a converted site has no such assignment. It needs no line
> window, and it catches both shapes. Comments and string/char literals are
> blanked (newline-preserving, so line numbers survive) before scanning —
> `src/` carries eleven comment lines that quote the antipattern while
> explaining why the code below no longer does it.
>
> **3. The workflow would never have run the gate.** `pr-metadata-validate.yml`
> had NO `src/**` in either `on.pull_request.paths` or `on.push.paths` — only
> `src/common/stub_backend.c` and `src/common/stub/**`. A PR adding a tenth
> unlocked claim in `src/backends/` would not have triggered the workflow at
> all, so registering the gate there without widening the trigger would have
> shipped a gate that never fires. `src/**` added to both lists. The same hole
> already applied to two gates hosted in that job: `check_stub_issues.py`
> (sweeps `src/backends/**/*_stub.c`) and `check_sw_fallback_tags.py` (sweeps
> `src/backends/**/sw_fallback.c`). Three gates, one trigger hole.
>
> **4. Task 1 Step 3's ztest was deliberately NOT written.** The plan concedes it
> "passes today" and that "the real regression protection for this issue is Task
> 3's gate". A test that cannot fail is not a test; the gate's own seeded-corpus
> tests pin `alp_slot_try_claim`'s contract properly, and
> `tests/unit/wdt_exclusivity/` (from #1637) exercises the helper end-to-end in a
> test that *did* fail before its fix. `tests/unit/slot_claim_sweep/` was skipped.
>
> **5. Site list re-derived and confirmed — nine, at the plan's exact lines.**
> The one citation that drifted is an allowlist entry, not a conversion site:
> the CAN filter claim is `src/backends/can/yocto_drv.c:662`, not `:603`. All
> four allowlist justifications were verified by reading the surrounding lock:
> `src/zephyr/handles.c:29` inside `k_mutex_lock(&kind##_lock, K_FOREVER)`,
> `src/yocto/peripheral_gpio.c:131` inside `pthread_mutex_lock(&g_irq.mu)`,
> `src/backends/can/yocto_drv.c:662` inside `pthread_mutex_lock(&d->lock)`, and
> `src/backends/can/testing_drv.c:346` a per-handle filter table.
>
> **6. `alp.lock` needs relocking, which the plan does not mention.** Adding the
> registry entry moves `digests.metadata`, so `stage_alp_lock` fails until
> `python3 scripts/west_commands/alp_lock.py --workspace .` is re-run and
> committed. The plan's Step 9 covers `gen_catalog.py` but not this.

## Background: what `alp_slot_try_claim` actually is

```c
/* src/common/alp_slot_claim.h:39-44 */
static inline bool alp_slot_try_claim(bool *in_use)
{
	bool expected = false;
	return __atomic_compare_exchange_n(
	    in_use, &expected, true, false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED);
}
```
```c
/* src/common/alp_slot_claim.h:54-57 */
static inline void alp_slot_release(bool *in_use)
{
	__atomic_store_n(in_use, false, __ATOMIC_RELEASE);
}
```

The defect being fixed is that `if (!flag) { flag = true; }` is two operations. Two threads opening concurrently can both read `false` and both proceed, so both get the same slot and their handles alias — one caller's `close()` then frees state the other is still using.

**There are two pool shapes in this tree and they take different conversions. Getting this wrong is the most likely way to break the sweep.**

**Shape B — `in_use` is the LAST member of the slot struct.** This is what the already-converted sites look like. The winner must zero only the bytes *ahead* of the flag, or the memset transiently un-claims the slot it just won:

```c
/* src/backends/spi/zephyr_drv.c:78-88 — the converted reference */
static alp_z_spi_side_t *_alloc_side(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(_sides); ++i) {
		/* Atomic claim (see alp_slot_claim.h): in_use is the last
		 * member, so the winner zeroes everything before it. */
		if (alp_slot_try_claim(&_sides[i].in_use)) {
			memset(&_sides[i], 0, offsetof(alp_z_spi_side_t, in_use));
			return &_sides[i];
		}
	}
	return NULL;
}
```

**Shape A — `in_use` is a SEPARATE `bool[]` array parallel to the pool.** **All eight array-shaped sites in this sweep are Shape A.** Because the flag does not live inside the struct, there is no partial-memset hazard and no `offsetof` is needed — a full `sizeof` memset after the claim is correct:

```c
	if (alp_slot_try_claim(&_x_in_use[i])) {
		memset(&_x_pool[i], 0, sizeof(_x_pool[i]));
		return &_x_pool[i];
	}
```

Do **not** copy the `offsetof` form into a Shape A site. It would not even compile — the Shape A slot struct has no `in_use` member for `offsetof` to reach — and it solves a hazard Shape A does not have. If you find yourself reaching for `offsetof`, you have misread which shape the site is; go back and look at where `in_use` is declared.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/backends/storage/zephyr_littlefs.c:61` | Modify: `_lfs_alloc()` → atomic claim | 1 |
| `src/backends/dac/zephyr_drv.c:92` | Modify: `_alloc_state()` → atomic claim | 1 |
| `src/backends/power/zephyr_pm_policy.c:199` | Modify: `_alloc_locks()` → atomic claim | 1 |
| `src/backends/ble/zephyr_drv.c:115` | Modify: `_conn_be_alloc()` → atomic claim | 1 |
| `src/backends/usb/zephyr_drv.c:64` | Modify: `_be_alloc()` → atomic claim | 1 |
| `src/backends/mproc/zephyr_drv.c:122` | Modify: `_shmem_be_alloc()` → atomic claim | 1 |
| `src/backends/mproc/zephyr_drv.c:145` | Modify: `_mbox_be_alloc()` → atomic claim | 1 |
| `src/backends/mproc/zephyr_drv.c:168` | Modify: `_hwsem_be_alloc()` → atomic claim | 1 |
| `src/backends/inference/tflm.cpp:307` | Modify: the singleton default-arena claim | 2 |
| `scripts/check_slot_claim_atomic.py` | Create: the gate | 3 |
| `metadata/quality-tasks-v1.json` | Modify: register the gate | 3 |
| `.github/workflows/pr-metadata-validate.yml` | Modify: run the gate in CI (job `validate`) | 3 |
| `tests/scripts/test_check_slot_claim_atomic.py` | Create: the gate's tests | 3 |

---

## Task 1: Convert the eight array-shaped pools

**Files:** the eight rows above.

**Interfaces:**
- Consumes: `alp_slot_try_claim()` / `alp_slot_release()` from `src/common/alp_slot_claim.h`.
- Produces: nothing. Task 3's gate is written against the *result*, not against an API this task defines.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/1630-slot-claim-atomic-sweep origin/dev
```

- [ ] **Step 2: Re-derive the site list yourself — do not trust this file's list**

The whole point of this issue is that hand-lists rot. Run the grep and reconcile against the eight rows above before editing anything:

```bash
grep -rnE "if \(!_?[a-z_]*in_use\[i\]\)|if \(![a-z_]+->in_use\)|if \(!_[a-z_]+\[i\]\.in_use\)" \
  src/ --include=*.c --include=*.cpp --include=*.h
```

If the grep returns a site not listed here, it is either a tenth site (add it) or one of the four documented exclusions below. If a listed site no longer matches, `dev` has moved — say so rather than editing blind.

**The four verified NOT-affected sites — do not convert these.** Each is already serialised by a held lock, so a CAS would be pure churn:

| Site | Why it is already safe |
|---|---|
| `src/zephyr/handles.c:29` | The whole loop runs inside `k_mutex_lock(&kind##_lock, K_FOREVER)` (`src/zephyr/handles.c:25`). |
| `src/yocto/peripheral_gpio.c:131` | Inside `pthread_mutex_lock(&g_irq.mu)`. |
| `src/backends/can/yocto_drv.c:603` | Inside `pthread_mutex_lock(&d->lock)`. |
| `src/backends/can/testing_drv.c:346` | A per-handle filter table in the testing backend, not a shared static pool. |

`src/zephyr/handles.c:29` carries a **different** defect — `kind##_slots[i] = (struct type){ 0 };` re-owning a slot while an operation may still be in flight. That belongs to the close-versus-op class tracked in #1644, not here. Do not "fix" it in this PR.

- [ ] **Step 3: Write the failing test**

Create `tests/unit/slot_claim_sweep/` following the `tests/unit/status_strings/` layout (`CMakeLists.txt` calling `find_package(Zephyr REQUIRED)` + `testcase.yaml` + `src/main.c`). The reachable assertion on `native_sim` is not a real data race — it is that a claimed slot is never handed out twice:

```c
#include <zephyr/ztest.h>
#include "common/alp_slot_claim.h"

ZTEST(alp_slot_claim, test_try_claim_is_exclusive)
{
	bool flag = false;

	zassert_true(alp_slot_try_claim(&flag), "first claim must win");
	zassert_true(flag, "winning claim must leave the flag set");
	zassert_false(alp_slot_try_claim(&flag), "second claim on a held slot must lose");

	alp_slot_release(&flag);
	zassert_false(flag, "release must clear the flag");
	zassert_true(alp_slot_try_claim(&flag), "a released slot must be claimable again");
	alp_slot_release(&flag);
}

ZTEST_SUITE(alp_slot_claim, NULL, NULL, NULL, NULL, NULL);
```

This test passes today — `alp_slot_try_claim` already works. It exists so the sweep has a green anchor and so the helper's contract is pinned. **The real regression protection for this issue is Task 3's gate, not a ztest**; a genuine two-thread race is not reproducible on `native_sim` on demand, and a test that cannot fail deterministically is worse than no test. Say exactly that in the PR body rather than implying the ztest proves the sweep.

- [ ] **Step 4: Run it and confirm it PASSES**

```bash
west twister -p native_sim/native/64 -T tests/unit/slot_claim_sweep --no-clean -v
```

- [ ] **Step 5: Convert `src/backends/storage/zephyr_littlefs.c:61`**

Current:

```c
static lfs_state_t *_lfs_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL; ++i) {
		if (!_lfs_in_use[i]) {
			memset(&_lfs_pool[i], 0, sizeof(_lfs_pool[i]));
			_lfs_in_use[i] = true;
			return &_lfs_pool[i];
		}
	}
	return NULL;
}
```

Replace the claim (note the order flips — claim first, then zero):

```c
static lfs_state_t *_lfs_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_STORAGE_LITTLEFS_HANDLE_POOL; ++i) {
		/* Atomic claim (see src/common/alp_slot_claim.h, issue #1115).
		 * in_use lives in a parallel array, not in the slot, so the
		 * winner may zero the whole slot after claiming it. */
		if (alp_slot_try_claim(&_lfs_in_use[i])) {
			memset(&_lfs_pool[i], 0, sizeof(_lfs_pool[i]));
			return &_lfs_pool[i];
		}
	}
	return NULL;
}
```

Add `#include "common/alp_slot_claim.h"` if the file does not already have it, and convert the matching free path to `alp_slot_release(&_lfs_in_use[i]);`.

This is the worst of the nine: two aliased handles onto one littlefs mount means two `lfs_t` states over one flash area, which is filesystem corruption on a persistent store rather than a lost handle.

- [ ] **Step 6: Convert the remaining seven, same shape**

Identical transformation, per site. Convert both the alloc **and** its free counterpart in each file:

- [ ] `src/backends/dac/zephyr_drv.c:92` — `_alloc_state()` / `_state_in_use[]`. Note this file currently sets the flag **before** zeroing (`_state_in_use[i] = true; _state_pool[i] = (zephyr_dac_state_t){ 0 };`); after conversion the claim does that, and the struct assignment stays.
- [ ] `src/backends/power/zephyr_pm_policy.c:199` — `_alloc_locks()` / `_lock_in_use[]`.
- [ ] `src/backends/ble/zephyr_drv.c:115` — `_conn_be_alloc()` / `_conn_be_in_use[]`.
- [ ] `src/backends/usb/zephyr_drv.c:64` — `_be_alloc()` / `_be_pool_in_use[]`.
- [ ] `src/backends/mproc/zephyr_drv.c:122` — `_shmem_be_alloc()` / `_shmem_be_in_use[]`; free at `:131-136`.
- [ ] `src/backends/mproc/zephyr_drv.c:145` — `_mbox_be_alloc()` / `_mbox_be_in_use[]`; free at `:154-159`.
- [ ] `src/backends/mproc/zephyr_drv.c:168` — `_hwsem_be_alloc()` / `_hwsem_be_in_use[]`; free at `:177-182`.

The three `mproc` pools are verbatim copies of each other. Convert all three in one pass; converting one and leaving two is the exact failure this issue exists to end.

- [ ] **Step 7: Confirm every free path releases atomically**

```bash
grep -rn "_in_use\[i\] = false" src/
```

Every hit is a non-atomic release that should become `alp_slot_release(&..._in_use[i]);`. A plain store is *less* dangerous than a plain claim (there is no lost-update window), but leaving it mixed means the next reader cannot tell converted from unconverted by reading the file — which is how the class survived twice.

- [ ] **Step 8: Format and run the full gate set**

```bash
clang-format -i src/backends/storage/zephyr_littlefs.c src/backends/dac/zephyr_drv.c \
                src/backends/power/zephyr_pm_policy.c src/backends/ble/zephyr_drv.c \
                src/backends/usb/zephyr_drv.c src/backends/mproc/zephyr_drv.c \
                tests/unit/slot_claim_sweep/src/main.c
git diff --exit-code
bash scripts/test-all.sh --target dev
```

- [ ] **Step 9: Commit**

```bash
git add src/backends/storage/zephyr_littlefs.c src/backends/dac/zephyr_drv.c \
        src/backends/power/zephyr_pm_policy.c src/backends/ble/zephyr_drv.c \
        src/backends/usb/zephyr_drv.c src/backends/mproc/zephyr_drv.c \
        tests/unit/slot_claim_sweep/
git commit -m "fix: claim the eight remaining backend pools atomically (#1115 residue)

Eight pools still ran the pre-#1115 check-then-set: two concurrent openers could
both read in_use false, both take the slot, and alias their handles -- so one
caller's close() frees state the other still holds. Worst site is the littlefs
mount pool, where aliasing means two lfs_t over one flash area.

All eight keep in_use in a parallel bool array rather than in the slot struct,
so the winner may memset the whole slot after claiming; no offsetof form is
needed here. Free paths converted to alp_slot_release() in the same pass."
```

---

## Task 2: Convert the singleton the grep cannot see

**Files:** `src/backends/inference/tflm.cpp:307`

**Interfaces:** consumes `alp_slot_try_claim()`; produces nothing.

**Why this is its own task.** The other eight are `flag[i]` inside a loop. This one is a bare scalar with no loop and no array subscript, so **every array-shaped grep in this issue's history walked straight past it** — which is exactly why it survived #1115. Filing and fixing it separately makes that visible instead of burying it as the ninth bullet.

Current:

```c
	} else {
		if (g_default_arena_in_use) {
			delete st;
			return ALP_ERR_NOMEM;
		}
		g_default_arena_in_use = true;
		st->arena_buf          = g_default_arena;
		st->arena_size         = kDefaultArenaBytes;
```

Two interpreters opening concurrently both read `g_default_arena_in_use == false`, both take `g_default_arena`, and then both run inference over **one shared arena** — which is silent corruption of model activations, not a clean `ALP_ERR_NOMEM`.

- [ ] **Step 1: Convert the claim**

```c
	} else {
		/* Atomic claim of the single shared default arena
		 * (src/common/alp_slot_claim.h, issue #1115).  A plain
		 * check-then-set let two interpreters share one arena and
		 * silently corrupt each other's activations. */
		if (!alp_slot_try_claim(&g_default_arena_in_use)) {
			delete st;
			return ALP_ERR_NOMEM;
		}
		st->arena_buf  = g_default_arena;
		st->arena_size = kDefaultArenaBytes;
```

- [ ] **Step 2: Convert the release**

Find where `g_default_arena_in_use` is set back to `false` (the close/destroy path) and make it `alp_slot_release(&g_default_arena_in_use);`. If there is **no** such site, that is a second defect — the default arena is never returned to the pool, so the second interpreter ever opened fails with `ALP_ERR_NOMEM` for the life of the process. Fix it here and say so in the PR body.

- [ ] **Step 3: Wrap the include — the header has NO `extern "C"` guard**

Verified: `src/common/alp_slot_claim.h` contains no `extern "C"` block and no `__cplusplus` guard, and `src/backends/inference/tflm.cpp` does not include it today.

`alp_slot_try_claim()` itself is `static inline` and would compile and link fine unwrapped. The trap is the rest of the header: `alp_slot_sleep_tick()`, `alp_handle_begin_close_blocking()`, `alp_handle_drain_blocking()` and `alp_handle_begin_close_selfaware()` are **out-of-line** declarations whose definitions are compiled in `src/common/alp_slot_claim.c` with C linkage. An unwrapped include declares them with C++ linkage, so this file links today only because it never calls one — a latent break for whoever adds the first call.

Wrap it:

```cpp
extern "C" {
#include "common/alp_slot_claim.h"
}
```

Do not "fix" this by adding an `extern "C"` guard to the header in this PR — that is a reasonable change but it touches every C consumer and does not belong in a concurrency fix. File it separately if you want it.

- [ ] **Step 4: Format, gate, commit**

```bash
clang-format -i src/backends/inference/tflm.cpp
git diff --exit-code
bash scripts/test-all.sh --target dev
git add src/backends/inference/tflm.cpp
git commit -m "fix(inference): claim the TFLM default arena atomically

g_default_arena_in_use was a plain check-then-set on a bare scalar -- no loop,
no array subscript, so every array-shaped grep in #1115's remediation missed it.
Two interpreters opening concurrently both won the single shared arena and ran
inference over each other's activations instead of one getting ALP_ERR_NOMEM."
```

---

## Task 3: The gate — make the tenth site impossible

**Files:**
- Create: `scripts/check_slot_claim_atomic.py`
- Modify: `metadata/quality-tasks-v1.json`
- Modify: `.github/workflows/pr-metadata-validate.yml`
- Test: `tests/scripts/test_check_slot_claim_atomic.py`

**Interfaces:**
- Produces: `find_problems(root: Path) -> list[str]`, imported directly by the test with no subprocess.

**This is the deliverable.** Tasks 1 and 2 close today's instances; this closes the class.

**Four sites move together or the PR is red before the gate ever runs.** `scripts/check_quality_registry.py` asserts that every `scripts/check_*.py` on disk appears exactly once in `metadata/quality-tasks-v1.json` — its failure strings are `<script>: on disk but missing from quality-tasks-v1.json` and `<script>: in registry but no such scripts/ file`. As of this writing there are 63 `scripts/check_*.py` on disk and 62 registry tasks (the registry excludes only `check_quality_registry.py` itself), 59 with `gate: true` and 14 with `ci: null`.

**Do NOT edit `scripts/test-all.sh`.** Its `REQUIRED_GATE_SCRIPTS` array is built at runtime from `python3 scripts/quality_tasks.py --gate-scripts`, which reads the registry. Registering in site 2 is what wires the gate into `bash scripts/test-all.sh`; hand-editing test-all.sh re-forks the list that single-sourcing exists to prevent.

- [ ] **Step 1: Write the failing test FIRST**

Create `tests/scripts/test_check_slot_claim_atomic.py`, patterned on `tests/scripts/test_check_bootstrap_manifest.py`:

```python
# SPDX-License-Identifier: Apache-2.0
"""Tests for scripts/check_slot_claim_atomic.py."""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))

from check_slot_claim_atomic import find_problems  # noqa: E402

CLEAN = """\
static thing_t *_alloc(void)
{
\tfor (size_t i = 0; i < ARRAY_SIZE(_pool); ++i) {
\t\tif (alp_slot_try_claim(&_in_use[i])) {
\t\t\tmemset(&_pool[i], 0, sizeof(_pool[i]));
\t\t\treturn &_pool[i];
\t\t}
\t}
\treturn NULL;
}
"""

DIRTY = """\
static thing_t *_alloc(void)
{
\tfor (size_t i = 0; i < ARRAY_SIZE(_pool); ++i) {
\t\tif (!_in_use[i]) {
\t\t\t_in_use[i] = true;
\t\t\treturn &_pool[i];
\t\t}
\t}
\treturn NULL;
}
"""


def _seed(root: Path, relpath: str, body: str) -> None:
    p = root / relpath
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_text(body, newline="")


def test_atomic_claim_passes(tmp_path: Path) -> None:
    _seed(tmp_path, "src/backends/thing/zephyr_drv.c", CLEAN)
    assert find_problems(tmp_path) == []


def test_check_then_set_is_reported(tmp_path: Path) -> None:
    _seed(tmp_path, "src/backends/thing/zephyr_drv.c", DIRTY)
    problems = find_problems(tmp_path)
    assert len(problems) == 1
    assert "src/backends/thing/zephyr_drv.c" in problems[0]
    assert "alp_slot_try_claim" in problems[0], "the message must name the fix"


def test_allowlisted_site_is_not_reported(tmp_path: Path) -> None:
    _seed(tmp_path, "src/zephyr/handles.c", DIRTY)
    assert find_problems(tmp_path) == []
```

Note `write_text(..., newline="")` — the repo has a `check_write_text_newline.py` gate requiring exactly that, and a test that violates it reddens `python-smoke`.

- [ ] **Step 2: Run the test and confirm it FAILS**

```bash
py -3 -m pytest tests/scripts/test_check_slot_claim_atomic.py -q
```

Expected: collection error — `No module named 'check_slot_claim_atomic'`.

- [ ] **Step 3: Write the gate**

Create `scripts/check_slot_claim_atomic.py`. Structure it as `find_problems(root) -> list[str]` plus a thin `main()` with `--root`, so the test needs no subprocess:

```python
#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Reject the unlocked check-then-set slot claim (issue #1630).

A static handle pool must claim its slot with alp_slot_try_claim()
(src/common/alp_slot_claim.h), whose compare-exchange lets exactly one
concurrent opener win.  A plain `if (!in_use) { in_use = true; }` is two
operations: two openers can both read false, both take the slot, and alias
their handles, so one caller's close() frees state the other still holds.

This class survived both #1115 and #629 because each was remediated from a
hand-written site list rather than from a grep.  This gate is what makes the
question a CI answer instead of a judgement call.

Allowlisted sites are already serialised by a held lock; converting them to a
CAS would be churn.  Each entry must say which lock, so the list cannot grow
by assertion.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

# path -> the lock that already serialises the claim.
ALLOWLIST: dict[str, str] = {
    "src/zephyr/handles.c": "k_mutex_lock(&kind##_lock, K_FOREVER) at src/zephyr/handles.c:25",
    "src/yocto/peripheral_gpio.c": "pthread_mutex_lock(&g_irq.mu)",
    "src/backends/can/yocto_drv.c": "pthread_mutex_lock(&d->lock)",
    "src/backends/can/testing_drv.c": "per-handle filter table, not a shared static pool",
}

# `if (!<something>in_use<something>)` -- the test half of the antipattern.
_TEST = re.compile(r"if\s*\(\s*!\s*[A-Za-z0-9_\.\[\]\->]*in_use[A-Za-z0-9_\.\[\]\->]*\s*\)")
# `<something>in_use<something> = true` -- the set half.
_SET = re.compile(r"[A-Za-z0-9_\.\[\]\->]*in_use[A-Za-z0-9_\.\[\]\->]*\s*=\s*true")

SUFFIXES = (".c", ".cpp", ".h")


def find_problems(root: Path) -> list[str]:
    problems: list[str] = []
    src = root / "src"
    if not src.is_dir():
        return problems
    for path in sorted(src.rglob("*")):
        if path.suffix not in SUFFIXES or not path.is_file():
            continue
        rel = path.relative_to(root).as_posix()
        if rel in ALLOWLIST:
            continue
        lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        for n, line in enumerate(lines, start=1):
            if line.lstrip().startswith(("*", "//", "/*")):
                continue  # a comment describing the antipattern is not the antipattern
            if not _TEST.search(line):
                continue
            # The set must follow within a short window to be this antipattern.
            window = "\n".join(lines[n : n + 4])
            if _SET.search(window):
                problems.append(
                    f"{rel}:{n}: unlocked check-then-set slot claim; "
                    f"use alp_slot_try_claim() from src/common/alp_slot_claim.h "
                    f"(issue #1630). If this site is already serialised by a held "
                    f"lock, add it to ALLOWLIST in {Path(__file__).name} with the "
                    f"lock named."
                )
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=".", help="repository root to scan")
    args = ap.parse_args()
    problems = find_problems(Path(args.root))
    for p in problems:
        print(p, file=sys.stderr)
    if problems:
        print(f"\n{len(problems)} unlocked slot claim(s) found.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
```

- [ ] **Step 4: Run the tests and confirm they PASS**

```bash
py -3 -m pytest tests/scripts/test_check_slot_claim_atomic.py -q
```

- [ ] **Step 5: Run the gate on the real tree — it must be CLEAN**

```bash
py -3 scripts/check_slot_claim_atomic.py
echo "exit=$?"
```

Expected: exit 0, no output — Tasks 1 and 2 converted every site. **If it reports anything, that is a site Tasks 1-2 missed**; fix the site, do not widen the allowlist. Widening the allowlist to make the gate green is the failure mode this whole plan exists to prevent, and the allowlist's per-entry "which lock" requirement is there to make that hard to do by accident.

- [ ] **Step 6: Register the gate**

Add one entry to `tasks` in `metadata/quality-tasks-v1.json`, **keeping the array sorted by `id`**:

```json
{
  "id": "slot-claim-atomic",
  "description": "Backend handle pools claim slots with alp_slot_try_claim(), not an unlocked check-then-set.",
  "runner": "check-script",
  "script": "scripts/check_slot_claim_atomic.py",
  "gate": true,
  "profiles": ["pr", "full", "release"],
  "output": "none",
  "ci": "pr-metadata-validate.yml:validate"
}
```

This is the right home: 35 of the 62 registered tasks already use `pr-metadata-validate.yml:validate`, and the gate needs only stdlib Python, which that job has. Do **not** use `pr-static-analysis.yml` — its only jobs are `clang-format-diff` and `cppcheck`, neither of which is a Python-gate sweep. `check_quality_registry.py` verifies the `ci` string by grepping that exact workflow's that exact job for the script name, so a wrong value fails the self-check.

- [ ] **Step 7: Add the workflow step**

Add a step to the `validate` job in `.github/workflows/pr-metadata-validate.yml` rather than creating a new workflow:

```yaml
      - name: slot-claim atomicity (#1630)
        run: python3 scripts/check_slot_claim_atomic.py
```

- [ ] **Step 8: Run the registry self-check**

```bash
py -3 scripts/check_quality_registry.py
echo "exit=$?"
```

Expected: exit 0. A failure here names which of the four sites you missed.

- [ ] **Step 9: Regenerate the catalog**

A new `scripts/check_*.py` drifts `metadata/catalog.json`:

```bash
python3 scripts/gen_catalog.py
git diff --stat metadata/catalog.json
```

- [ ] **Step 10: Full gate set, then commit**

```bash
bash scripts/test-all.sh --target dev
git add scripts/check_slot_claim_atomic.py metadata/quality-tasks-v1.json \
        .github/workflows/pr-metadata-validate.yml \
        tests/scripts/test_check_slot_claim_atomic.py metadata/catalog.json
git commit -m "ci: gate the unlocked check-then-set slot claim

This class survived #1115 and #629 because both were remediated from a
hand-written site list rather than a grep, so a tenth site could be written
the day after each closed. The gate rejects the antipattern anywhere under
src/, with a four-entry allowlist for sites already serialised by a held lock
-- each entry naming which lock, so the list cannot grow by assertion."
```

---

## Opening the PRs

Two PRs, both `--base dev`:

1. **Tasks 1 + 2** — the sweep. `Closes #1630.` on its own line.
2. **Task 3** — the gate. `Refs #1630.` It could ride in PR 1, but splitting keeps a mechanical nine-site diff reviewable separately from a new CI blocker, and lets the gate land even if a site conversion needs rework.

If they ship as one PR instead, `Closes #1630.` goes on that one and nothing else changes.

Labels: `bug`, `area:drivers`, `area:portability` for the sweep; add `area:ci` for the gate PR. `area:npu` on whichever PR carries the TFLM change. Milestone `v0.17.0`.

**No bench time required.** Every site is a pure concurrency-correctness change with no hardware behaviour, and the gate is Python. This is the one plan in the campaign that can go from branch to merge without a reservation.
