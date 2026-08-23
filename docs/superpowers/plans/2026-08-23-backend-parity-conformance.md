# Backend-Parity Conformance Suite — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue: #1635** (`bug`, `area:drivers`, `area:portability`, `area:ci`, milestone `Backlog`)

**Goal:** Make the SoM-swap promise testable — one portable call, one contract — by adding a harness that runs the same call against every registered backend of a class and fails on *divergence*, then fixing the 14 sites it is seeded with.

**Architecture:** The harness is the deliverable; the 14 fixes are its first patch series. Task 1 builds the harness and lands it red, listing the divergences it finds. Tasks 2-4 turn it green one family at a time. Ordering matters: a suite written after the fixes would be tuned to pass rather than tuned to detect.

**Tech Stack:** C (clang-format 22.x, tabs), Zephyr, ztest, twister on `native_sim/native/64`.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections first.

## Global Constraints

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 280 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` green before `gh pr create`.
- clang-format **22.x** on every changed `.c`/`.h` including test files.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result.
- No AI attribution anywhere.
- **No new CI job.** The suite runs inside the existing `twister` shards on `native_sim/native/64`.

---

## The measured starting state

`tests/zephyr/conformance/` already exists and is well built, but it does not — and structurally cannot — test what this issue is about.

**Two mutually exclusive app images, deliberately.** `tests/zephyr/conformance/CMakeLists.txt` is a hard `if/else`:

```cmake
if(CONFIG_ALP_SDK_TESTING)
    target_sources(app PRIVATE
        src/behavior_gpio.c
        src/behavior_uart.c
        src/behavior_i2c.c
        src/behavior_spi.c
        src/behavior_adc.c
        src/behavior_can.c
        src/behavior_storage.c
    )
else()
    target_sources(app PRIVATE
        src/main.c
    )
endif()
```

The comment above it explains why, and it is correct reasoning worth preserving: `main.c`'s rows assume the real/emulated backend (gpio Case B expects `alp_gpio_open(99)` to **fail**), while `CONFIG_ALP_SDK_TESTING`'s priority-255 wildcard doubles must open **any** instance. The two "test different, mutually-incompatible backend selections for the same class and must never run in one process."

**What each image covers.** `src/main.c` holds a 16-row `conf_classes[]` table (`:754-934`) driven by 8 generic `ZTEST`s (`:940-1143`), with expectations derived from `alp_has()` rather than hardcoded (`conf_must_open` / `conf_must_degrade`, `:215-237`). The seven `behavior_*.c` files drive the `testing_drv` doubles.

**Neither runs one call against two backends.** That is the gap.

### The gap is real, and so is the mechanism to close it

Two backends of one class **are** linked together on `native_sim`:

```
zephyr/CMakeLists.txt:1442    src/backends/uart/zephyr_drv.c
zephyr/CMakeLists.txt:1444    src/backends/uart/sw_fallback.c
zephyr/CMakeLists.txt:1570    src/backends/uart/testing_drv.c   (CONFIG_ALP_SDK_TESTING only)
```

and the registry is already walkable:

```c
/* include/alp/backend.h:187, :210, :218 */
const alp_backend_t *alp_backend_select(const char *class_name, const char *silicon_ref);
const alp_backend_t *
alp_backend_select_next(const char *class_name, const char *silicon_ref, const alp_backend_t *prev);
size_t alp_backend_count(const char *class_name);
```

### The design decision this plan turns on

**There is no way to make `alp_<class>_open()` use a chosen backend.** The dispatchers hardcode the winner:

```c
/* src/uart_dispatch.c:63 */
	const alp_backend_t *be = alp_backend_select("uart", ALP_SOC_REF_STR);
```

`grep -rn "backend_force\|force_backend\|ALP_BACKEND_OVERRIDE" src/ include/ tests/` returns nothing. So the harness cannot simply loop backends and call the public API.

Two options, and the choice shapes everything below:

1. **Add a test-only backend pin** to `src/backend.c` so `alp_backend_select()` can be forced to return a named entry. Tests the real portable path end to end — but puts a test seam into the production dispatch path of an SDK that ships to customers. **Rejected.**
2. **Enumerate the registry and call each backend's ops table directly**, bypassing `alp_<class>_open()`. No production change whatsoever. **Chosen.**

Option 2's honest limitation, which the plan must not paper over: it tests **backend-versus-backend** parity, not dispatcher behaviour. Two of the 14 seeded sites are *dispatcher* divergences living inside a single file —

- `src/storage_dispatch.c:157-158` returns `ALP_ERR_NOT_READY` for a write on a `read_only` handle while `:178-179` returns `ALP_ERR_INVAL` for erase on the same handle for the same reason;
- the PWM out-of-range-channel code differs across `pwm/yocto_drv.c:212` (`ALP_ERR_OUT_OF_RANGE`), `pwm/gd32_bridge.c:69` (`ALP_ERR_INVAL`), and `pwm/zephyr_drv.c:113-114` (**both**).

— and the harness will not catch either. They are fixed directly in Task 3 and the harness gains rows for them only where a backend, not the dispatcher, owns the answer. Say this in the PR rather than implying the suite covers all 14.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `tests/zephyr/conformance/src/behavior_parity.c` | Create: the cross-backend divergence harness | 1 |
| `tests/zephyr/conformance/CMakeLists.txt` | Modify: add the file to the `else()` (non-TESTING) branch | 1 |
| `tests/zephyr/conformance/testcase.yaml` | Modify: a `parity` scenario if the default `prj.conf` lacks the sw_fallback opt-in | 1 |
| `src/backends/qenc/gd32_bridge.c:28` | Modify: bound `encoder_id` at open | 2 |
| `src/backends/adc/alif_e8.c:259`, `alif_e7.c:237` | Modify: full three-part vendor guard | 2 |
| `src/backends/ext/renesas/power.c:48` | Modify: add the missing `vendor != NULL` half | 2 |
| `src/backends/wdt/zephyr_drv.c:72` | Modify: release the installed channel on a failed `wdt_setup` | 2 |
| `src/backends/storage/zephyr_flash.c`, `zephyr_littlefs.c`, `src/backends/display/zephyr_drv.c` | Modify: honour `allow_unsafe_write` / `allow_modeset` | 2 |
| `src/backends/pwm/{yocto_drv,gd32_bridge,zephyr_drv}.c` | Modify: one status for out-of-range channel | 3 |
| `src/storage_dispatch.c:157-178` | Modify: one status for `read_only` | 3 |
| `src/backends/uart/sw_fallback.c:76-86` | Modify: empty read returns `ALP_ERR_TIMEOUT` | 3 |
| `src/backends/storage/zephyr_flash.c:92`, `zephyr_littlefs.c:179` | Modify: report the real erase granule | 3 |
| `src/backends/audio/zephyr_drv.c:351`, `i2s/zephyr_drv.c:214`, `mqtt/yocto_drv.c:399` | Modify: the `UINT32_MAX` timeout convention | 4 |

---

## Task 1: The parity harness

**Files:** `tests/zephyr/conformance/src/behavior_parity.c` (create), `CMakeLists.txt`, `testcase.yaml`.

**Interfaces:**
- Consumes: `alp_backend_count()`, `alp_backend_select()`, `alp_backend_select_next()` from `include/alp/backend.h`, and the per-class `alp_<class>_ops_t` structs from `src/backends/<class>/<class>_ops.h`.
- Produces: nothing other tasks call. Tasks 2-4 are validated *by* it, not linked against it.

**Land this red.** The point of writing the harness first is that it reports the divergences rather than being tuned around them. Its first run should fail, and the PR body should list exactly what it found.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b feat/1635-backend-parity-conformance origin/dev
```

- [ ] **Step 2: Confirm two backends really are enumerable at runtime**

Before writing the harness, prove the premise. Add a throwaway `ZTEST` to `tests/zephyr/conformance/src/main.c` — you will delete it in Step 4 — that prints what the registry holds:

```c
ZTEST(alp_conformance, tmp_probe_backend_counts)
{
	static const char *const classes[] = { "uart", "i2c", "spi", "storage", "adc" };
	for (size_t i = 0; i < ARRAY_SIZE(classes); ++i) {
		size_t n = alp_backend_count(classes[i]);
		printk("class %-8s backends=%zu\n", classes[i], n);
		const alp_backend_t *be = alp_backend_select(classes[i], ALP_SOC_REF_STR);
		while (be != NULL) {
			printk("    vendor=%s priority=%d\n", be->vendor, be->priority);
			be = alp_backend_select_next(classes[i], ALP_SOC_REF_STR, be);
		}
	}
	zassert_true(true);
}
```

```bash
west twister -p native_sim/native/64 -T tests/zephyr/conformance --no-clean -v
```

**Read the output before going further.** If every class reports `backends=1`, the `sw_fallback` opt-in is not enabled in this scenario's `prj.conf` — `zephyr/CMakeLists.txt:655` calls it "gated on the native_sim opt-in". Find that Kconfig and add it via a `parity` scenario in `testcase.yaml` with `extra_args: ["EXTRA_CONF_FILE=prj_parity.conf"]`, exactly as the existing scenarios add configs. **If you cannot get two backends into one image for any class, stop and report that** — the whole harness design depends on it and the fallback is a different plan, not a workaround.

- [ ] **Step 3: Write the harness**

Create `tests/zephyr/conformance/src/behavior_parity.c`. The core idea: for each row, run the same call against every enumerated backend and compare results **to each other**, not to a hardcoded constant — so the test fails on divergence and does not encode a guess about which sibling is right.

```c
/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cross-backend parity harness (issue #1635).
 *
 * The SoM-swap promise is that one <alp/*> call behaves the same on every
 * registered backend of its class.  main.c checks each class against the
 * capability layer; the behavior_*.c suites drive the test doubles.  Neither
 * runs ONE call against TWO backends, which is where the divergences live.
 *
 * There is deliberately no way to make alp_<class>_open() use a chosen
 * backend -- the dispatchers call alp_backend_select() and take the winner
 * (src/uart_dispatch.c:63).  Rather than add a test-only pin to production
 * dispatch, this harness enumerates the registry and drives each backend's
 * ops table directly.  Consequence, stated plainly: it tests backend-vs-
 * backend parity, NOT dispatcher behaviour.  Divergences that live inside a
 * dispatcher (src/storage_dispatch.c:157 vs :178) are invisible here and are
 * fixed directly.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/uart/uart_ops.h"

#define PARITY_MAX_BACKENDS 4

/** Collect every backend registered for @p class_name, best-ranked first. */
static size_t parity_collect(const char *class_name, const alp_backend_t **out, size_t max)
{
	size_t               n  = 0;
	const alp_backend_t *be = alp_backend_select(class_name, ALP_SOC_REF_STR);
	while (be != NULL && n < max) {
		out[n++] = be;
		be       = alp_backend_select_next(class_name, ALP_SOC_REF_STR, be);
	}
	return n;
}

/**
 * Assert every backend of a class answered identically.
 *
 * Reports which vendor disagreed, not just that something did -- a parity
 * failure whose message does not name the outlier costs an hour to triage.
 */
static void parity_assert_same(const char                  *what,
                               const alp_backend_t *const  *bes,
                               const alp_status_t          *got,
                               size_t                       n)
{
	for (size_t i = 1; i < n; ++i) {
		zassert_equal(got[i], got[0],
		              "%s: backend '%s' returned %s but backend '%s' returned %s",
		              what, bes[i]->vendor, alp_status_name(got[i]), bes[0]->vendor,
		              alp_status_name(got[0]));
	}
}

/* ---- Row: UART empty read with a finite timeout ------------------- */
/* sw_fallback returns ALP_OK having written nothing; zephyr_drv returns
 * ALP_ERR_TIMEOUT.  A caller that checks only the status reads its own
 * uninitialised stack as received data, and sw_fallback is what every
 * native_sim and plain-CMake build links. */
ZTEST(alp_parity, test_uart_empty_read_agrees)
{
	const alp_backend_t *bes[PARITY_MAX_BACKENDS];
	size_t               n = parity_collect("uart", bes, PARITY_MAX_BACKENDS);
	if (n < 2) {
		ztest_test_skip(); /* only one uart backend in this image */
	}

	alp_status_t got[PARITY_MAX_BACKENDS];
	for (size_t i = 0; i < n; ++i) {
		const alp_uart_ops_t     *ops = (const alp_uart_ops_t *)bes[i]->ops;
		alp_uart_backend_state_t  st  = { 0 };
		alp_capabilities_t        caps = { 0 };
		alp_uart_config_t         cfg  = ALP_UART_CONFIG_DEFAULT(0u);
		uint8_t                   buf[4];

		if (ops == NULL || ops->open == NULL || ops->read == NULL) {
			ztest_test_skip(); /* partial ops table -- see issue #1641 */
		}
		if (ops->open(&cfg, &st, &caps) != ALP_OK) {
			ztest_test_skip(); /* this backend cannot open instance 0 here */
		}
		size_t n_read = 0;
		got[i]        = ops->read(&st, buf, sizeof(buf), &n_read, 10u);
		if (ops->close != NULL) {
			ops->close(&st);
		}
	}
	parity_assert_same("uart empty read, 10 ms timeout", bes, got, n);
}

ZTEST_SUITE(alp_parity, NULL, NULL, NULL, NULL, NULL);
```

**Two things to verify against the tree before this compiles**, because they were not confirmed while writing this plan and a wrong guess here wastes a build cycle:

- The exact `alp_uart_ops_t` member names and the `read` signature — open `src/backends/uart/uart_ops.h` and match it. The campaign index records the vtable verbs as `open, write, read, close`, but the parameter lists were not captured.
- Whether `ALP_UART_CONFIG_DEFAULT` exists and takes an instance id, mirroring `ALP_I2C_CONFIG_DEFAULT(id)` (`include/alp/peripheral.h:20` in the i2c block). If it does not, construct the config literally.

The `#include "backends/uart/uart_ops.h"` path assumes the conformance app's include dirs reach `src/`. If it does not resolve, add the include directory in `tests/zephyr/conformance/CMakeLists.txt` rather than reaching with `../../../`.

- [ ] **Step 4: Delete the throwaway probe from Step 2**

- [ ] **Step 5: Wire the file into the non-TESTING image**

In `tests/zephyr/conformance/CMakeLists.txt`, add to the `else()` branch **only**:

```cmake
else()
    target_sources(app PRIVATE
        src/main.c
        src/behavior_parity.c
    )
endif()
```

It must not join the `CONFIG_ALP_SDK_TESTING` branch: the priority-255 wildcard doubles register for every class and would rank above the real backends, so the harness would compare a double against a double and pass vacuously. Extend that file's existing explanatory comment to say so — the next person will otherwise "helpfully" add it to both lists.

- [ ] **Step 6: Run it and record what it finds**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/conformance --no-clean -v
```

Expected: `test_uart_empty_read_agrees` **FAILS**, naming `sw_fallback` and the Zephyr backend and their two different statuses. That failure message is the deliverable of this task — paste it into the PR body verbatim.

- [ ] **Step 7: Add the remaining rows the harness can actually reach**

From the issue's seed list, the rows expressible as backend-vs-backend on `native_sim`:

- [ ] out-of-range instance id at `open()`
- [ ] `NULL` payload with a non-zero length
- [ ] `timeout_ms == UINT32_MAX` treated as wait-forever
- [ ] `get_info()` granule fields non-zero and self-consistent (storage)

Skip-mark, with a comment, any row whose backends are not linked on `native_sim` — the GD32-bridge and Alif backends need real silicon. A skip that says why is fine; a silently absent row is not.

- [ ] **Step 8: Format, gate, commit**

```bash
clang-format -i tests/zephyr/conformance/src/behavior_parity.c
git diff --exit-code
bash scripts/test-all.sh --target dev
```

`test-all.sh` will now be **red** on the new suite, by design. Commit anyway and open the PR as a draft, or gate the new scenario behind a Kconfig that Task 4's final commit flips on — decide which and say so in the PR. Do not make it pass by weakening an assertion.

```bash
git add tests/zephyr/conformance/src/behavior_parity.c \
        tests/zephyr/conformance/CMakeLists.txt tests/zephyr/conformance/testcase.yaml
git commit -m "test(conformance): add a cross-backend parity harness

Nothing in the tree ran one portable call against two registered backends and
diffed the status, so 14 sites where sibling backends disagree went unnoticed.

The harness enumerates the registry with alp_backend_select_next() and drives
each backend's ops table directly, comparing results to each other rather than
to a hardcoded constant -- it fails on divergence, not on a guess about which
sibling is right. It deliberately does not add a backend pin to production
dispatch, so dispatcher-internal divergences stay out of scope and are fixed
directly."
```

---

## Task 2: Family A — one sibling validates, the other does not

Six sites. Each is a small change; the value is that they are one class of defect.

- [ ] **Step 1: `src/backends/qenc/gd32_bridge.c:28` — bound the instance id**

`br_open` stores `st->encoder_id = cfg->encoder_id;` unbounded, then `:39` narrows it: `s = gd32g553_qenc_read(ctx, (uint8_t)st->encoder_id, pos_out);`. `encoder_id == 256` aliases to encoder **0** and reports `ALP_OK` — a silent wrong-device read.

Every sibling GD32-bridge backend already bounds at open: `src/backends/counter/gd32_bridge.c:33`, `src/backends/adc/gd32_bridge.c:79`, `src/backends/dac/gd32_bridge.c:72`, `src/backends/pwm/gd32_bridge.c:68`. Read one of them and match its form and its status code exactly — do not invent a third convention.

- [ ] **Step 2: `src/backends/adc/alif_e8.c:259` and `alif_e7.c:237` — the full vendor guard**

Both do `if (strcmp(h->backend->vendor, "alif") != 0)` with no `h->backend == NULL` and no `h->backend->vendor == NULL` check, then deref `h->state.be_data` unchecked at `:265` / `:243` and write through it with no `alp_handle_op_enter`.

Six of the seven `ext/` vendor gates carry the full three-part guard — `ext/alif/camera.c:62-63`, `ext/alif/storage.c:26-27`, `ext/deepx/inference.c:40-41`, `ext/nxp/storage.c:29-30`, `ext/renesas/camera.c:51-52`, `ext/renesas/inference.c:43-44`. Copy that shape.

The missing `alp_handle_op_enter` is a lifetime defect, not a validation one — it belongs to #1644. Add the NULL guards here and **note** the `op_enter` gap in the PR with a `Refs #1644`, rather than fixing two classes in one diff.

- [ ] **Step 3: `src/backends/ext/renesas/power.c:48` — the seventh gate**

Has the `handle->backend == NULL` half, lacks the `vendor != NULL` half. One line.

- [ ] **Step 4: `src/backends/wdt/zephyr_drv.c:72` — release the channel on failure**

A failed `wdt_setup` returns straight out, leaking the timeout channel installed at `:68-70`. The Yocto sibling centralises exactly this teardown in `_disarm_and_close` (`src/backends/wdt/yocto_drv.c:120`, added for #760); there is no Zephyr counterpart.

**Coordinate with Plan 7** (`2026-08-23-watchdog-config.md`, issue #1637) — it restructures this same function. Whichever lands second rebases; do not both edit `z_open` in parallel.

- [ ] **Step 5: The two safety gates honoured on only one OS**

`src/backends/storage/zephyr_flash.c` and `zephyr_littlefs.c` never read `alp_storage_config_t.allow_unsafe_write`; `src/backends/display/zephyr_drv.c` never reads `alp_display_config_t.allow_modeset`. Both are honoured **only** by the Yocto backends (`storage/yocto_drv.c:249`, `:358`, `:405`; `display/yocto_drv.c:333`).

So the same defaulted config that is refused on A55 Linux is silently accepted on M55/M33. That is the SoM-swap promise inverted: the customer's protection disappears when they move to the smaller part.

Read the Yocto implementation and mirror its refusal, including its status code.

- [ ] **Step 6: Format, gate, commit**

```bash
clang-format -i $(git diff --name-only --diff-filter=M | grep -E '\.(c|h)$')
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "fix: six sites where one backend validates and its sibling does not

qenc/gd32_bridge accepted an unbounded encoder_id and narrowed it with a
(uint8_t) cast, so encoder 256 read encoder 0 and reported ALP_OK. The two Alif
ADC vendor gates dereferenced h->backend->vendor without either NULL check its
six ext/ siblings carry. ext/renesas/power.c had half the guard. wdt/zephyr_drv
leaked an installed timeout channel when wdt_setup failed.

allow_unsafe_write and allow_modeset were honoured only by the Yocto backends,
so a config refused on A55 was silently accepted on M55/M33."
```

---

## Task 3: Family B — divergent status for the same condition

Five sites. **Each needs a decision about which sibling is right**, and the decision belongs in the PR body, not in a commit message alone.

- [ ] **Step 1: PWM out-of-range channel — pick one status**

`ALP_ERR_OUT_OF_RANGE` at `pwm/yocto_drv.c:212`, `ALP_ERR_INVAL` at `pwm/gd32_bridge.c:69`, and **both** at `pwm/zephyr_drv.c:113-114`.

`ALP_ERR_OUT_OF_RANGE` is the better answer — an instance id outside the SoC's channel count is exactly what the status names — and it matches what the two dispatcher-level gates already stamp for the same condition elsewhere. Check `include/alp/pwm.h`'s documented `@return` list first; if it names only one of the two, that settles it without an opinion.

- [ ] **Step 2: `src/storage_dispatch.c:157-178` — one file, two codes**

`:157-158` returns `ALP_ERR_NOT_READY` for a write on a `read_only` handle; `:178-179` returns `ALP_ERR_INVAL` for erase on the same handle for the same reason. Same handle, same cause, two codes, inside one function pair.

This is a **dispatcher** divergence, invisible to Task 1's harness. Fix it directly and note in the PR that the harness does not cover it.

- [ ] **Step 3: `src/backends/uart/sw_fallback.c:76-86` — the dangerous one**

Returns `ALP_OK` having written nothing, where `zephyr_drv.c:163` returns `ALP_ERR_TIMEOUT`. A caller that checks only the status reads **its own uninitialised stack** as received data — and `sw_fallback` is what every `native_sim` and plain-CMake build links, so this is the shape a new user meets first.

`ALP_ERR_TIMEOUT` is right, and it agrees with `alp_uart_read`'s documented whole-call semantics (`include/alp/peripheral.h:902-917`, including `timeout_ms == 0` as a single non-blocking poll). This fix makes Task 1's `test_uart_empty_read_agrees` go green.

- [ ] **Step 4: Storage erase granule**

`storage/zephyr_flash.c:92` reports `info->erase_size = 1u;` and `zephyr_littlefs.c:179` does the same, where `storage/yocto_drv.c:308` reports the device's real `info.erasesize`. `include/alp/storage.h:224` tells the caller both bounds MUST align to `erase_size` — so a caller that obeys the reported `1` gets every erase rejected by the underlying `flash_area_erase`.

Report the real granule from the flash-area / littlefs geometry. This one is worth a bench check on E1M-AEN801 if a session is scheduled, because the value comes from the flash driver.

- [ ] **Step 5: The two that are documented divergences, not bugs**

- `include/alp/tmu.h:36-42` already documents that a domain error is `ALP_ERR_OUT_OF_RANGE` on the V2N CORDIC backend and `ALP_OK` + NaN on the libm fallback, across all 12 functions. **Documented is not the same as fine** — it is still two behaviours behind one portable call — but changing it is an API decision, not a parity fix. Leave the code, add a harness row that asserts the *documented* split so a third behaviour cannot appear, and open a follow-up.
- The three inference backends report tensor rank three ways: `inference_drpai.cpp:412` hardcodes `out->rank = 0u;`, `inference_ort.cpp:651` truncates to 4 having pre-checked dims against `UINT16_MAX`, `inference_deepx.cpp:147-149` truncates to 4 **and** casts `int64_t` to `uint16_t` unchecked. The unchecked cast is a real defect; the rank contract is a design gap. Fix the cast here, and open a follow-up for the contract rather than inventing one in a parity PR.

- [ ] **Step 6: Format, gate, commit**

```bash
clang-format -i $(git diff --name-only --diff-filter=M | grep -E '\.(c|h|cpp)$')
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "fix: one status per condition across sibling backends

uart/sw_fallback returned ALP_OK on an empty read where zephyr_drv returned
ALP_ERR_TIMEOUT, so a caller checking only the status read its own
uninitialised stack as received data -- and sw_fallback is what every
native_sim and plain-CMake build links.

storage/zephyr_flash and zephyr_littlefs reported erase_size = 1 where the
Yocto backend reports the real granule, so a caller obeying storage.h:224's
alignment instruction had every erase rejected. PWM out-of-range and storage
read_only each had two codes for one condition."
```

---

## Task 4: Family C — the timeout convention

Three sites. The correct form is already in the tree three times:

```c
/* src/backends/camera/zephyr_video.c:291, mirrored at
 * camera/alif_isp_pico.c:336 and camera/v2n_n44_isp.c:267 */
	k_timeout_t t = (timeout_ms == UINT32_MAX) ? K_FOREVER : K_MSEC(timeout_ms);
```

- [ ] **Step 1: `src/backends/audio/zephyr_drv.c:351`**

```c
	int err = dmic_read(be->dev, 0, &block, &got, (int32_t)timeout_ms);
```

Any `timeout_ms > INT32_MAX` becomes negative — i.e. wait forever — including values a caller meant as a long-but-finite deadline. Clamp, and map `UINT32_MAX` to the driver's own forever.

- [ ] **Step 2: `src/backends/i2s/zephyr_drv.c:214`**

`K_MSEC(timeout_ms)` with no `UINT32_MAX` case, so a caller asking for "forever" gets a finite ~49.7-day timeout. Apply the camera form.

**Coordinate with Plan 1 Task 1** (`2026-08-23-critical-memory-corruption.md`, issue #1619) — it edits `z_write` in this same file. Whichever lands second rebases.

- [ ] **Step 3: `src/backends/mqtt/yocto_drv.c:399`**

```c
	rc = mosquitto_loop(be->mosq, (int)timeout_ms, 1);
```

Omits the `INT_MAX` clamp that the same file's `y_loop` applies 65 lines later at `:464`. Copy it — the correct code is already in the file.

- [ ] **Step 4: Add the harness row**

With all three fixed, add the `timeout_ms == UINT32_MAX` row to `behavior_parity.c` from Task 1 Step 7 if it was skip-marked. It should now pass across backends.

- [ ] **Step 5: The suite must now be GREEN**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/conformance --no-clean -v
bash scripts/test-all.sh --target dev
```

If Task 1's scenario was gated behind a Kconfig to keep `dev` green, flip it on in this commit — that is what makes the gate real. A harness that stays disabled is a file, not a gate.

- [ ] **Step 6: Format, gate, commit**

```bash
clang-format -i src/backends/audio/zephyr_drv.c src/backends/i2s/zephyr_drv.c \
                src/backends/mqtt/yocto_drv.c tests/zephyr/conformance/src/behavior_parity.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "fix: one timeout convention -- UINT32_MAX means forever everywhere

audio/zephyr_drv cast timeout_ms to int32_t, turning anything above INT32_MAX
into a negative, i.e. wait-forever. i2s/zephyr_drv had no UINT32_MAX case, so
'forever' became a finite ~49.7-day timeout. mqtt/yocto_drv omitted the INT_MAX
clamp its own y_loop applies 65 lines below.

The correct form already existed three times in camera/*. Enables the parity
suite's UINT32_MAX row."
```

---

## Opening the PRs

Four PRs, all `--base dev`, in task order — Task 1 must land (even red/gated) before 2-4 have anything to turn green.

- Task 1: `Refs #1635.` Labels `enhancement`, `area:ci`, `area:portability`.
- Task 2: `Refs #1635.` Labels `bug`, `area:drivers`.
- Task 3: `Refs #1635.` Labels `bug`, `area:drivers`, `area:portability`.
- Task 4: `Closes #1635.` Labels `bug`, `area:drivers`.

**Bench:** only the storage erase-granule change (Task 3 Step 4) genuinely wants silicon, and only because the value comes from the flash driver. Batch it into an E1M-AEN801 session scheduled for another plan rather than reserving for it alone. Everything else is verifiable on `native_sim`.

**Two follow-ups this plan deliberately does not do:** the `<alp/tmu.h>` documented two-behaviour split, and the three-way tensor-rank contract across the inference backends. Both are API decisions that need an owner, not parity patches.
