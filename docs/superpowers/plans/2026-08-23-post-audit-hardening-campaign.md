# Post-Audit Hardening Campaign — Index

> **This is a campaign index, not an executable plan.** It decomposes the
> 2026-08-23 standing-code audit of `src/` + `include/alp/` into independent
> sub-plans, fixes their order, and records the dependencies between them.
> Each sub-plan listed below is written separately under
> `docs/superpowers/plans/2026-08-23-<slug>.md` and is independently
> executable, testable, and mergeable. Execute sub-plans, not this file.

**Source:** standing-code audit of `src/` (298 files, 62149 LOC) and
`include/alp/` (148 headers, 25956 LOC) read at commit `8533874b`.
126 raw findings, 13 refuted by an adversarial pass, **113 confirmed**
(3 critical / 17 high / 78 medium / 15 low), plus a feature-gap review that
produced 27 grounded proposals from 34 (4 killed as WRONG-LAYER /
CONTRADICTS-REPO / SPECULATIVE, 3 unreviewed).

**Goal:** close the three memory-corruption criticals, then convert the three
systemic weaknesses the audit identified from "discipline that must hold 33
times" into "invariants a gate enforces once".

---

## Global Constraints

Every task in every sub-plan inherits these. Values are verbatim.

- **Base branch is `dev`.** Never `--base main`. Verify with
  `git merge-base HEAD origin/dev` before every `gh pr create`.
- **Feature branches are mandatory.** `dev` is the target, never the
  workbench. Branch name `fix/<topic>` or `feat/<topic>`.
- **The full local gate set must be green before `gh pr create`:**
  `bash scripts/test-all.sh --target dev`. Not a subset. Not `--quick`.
- **clang-format is 22.x** — tabs, Consecutive alignment, BinPack off. Format
  every changed `.c`/`.h` *including test files*, not just the header you were
  thinking about.
- **A new public symbol or macro is an ABI change** — regenerate the ABI
  snapshot or `check · generated files in sync` goes red.
- **After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and
  commit the result** even if the change did not touch it — `metadata/catalog.json`
  is chronically stale on `dev` and the red is inherited otherwise.
- **Never hand-edit a generated file.** Fix the generator under `scripts/` and
  regenerate. Every hardware fact has one source under `metadata/`.
- **No legacy/ABI compat shims, no deprecation tombstones.** There are no
  active customers; breaking changes are free now and impossible after 1.0.
- **No AI attribution anywhere** — no `Co-Authored-By: Claude`, no "Generated
  with" banner, no `claude.ai/code/session_<id>` URL in a commit message, PR
  body, or issue. A PR body becomes a public squash-commit message permanently.
- **Bench-before-merge** on any task marked `needs-silicon`. `native_sim`
  passing is not evidence that an ISR fires on an M55.

---

## Verification infrastructure (shared by every sub-plan)

Mapped 2026-08-23 so no task has to rediscover it.

**ztest — the form every new peripheral test uses.** Suite declared once per
directory, tests registered against it from sibling files:
```c
/* tests/zephyr/peripheral/src/main.c:52 */
ZTEST_SUITE(alp_peripheral, NULL, NULL, NULL, NULL, NULL);
```
```c
/* tests/zephyr/peripheral/src/dac.c — complete test, the pattern to copy */
#include <zephyr/ztest.h>
#include "alp/dac.h"
#include "alp/peripheral.h"
#include "alp/soc_caps.h"

ZTEST(alp_peripheral, test_dac_null_cfg)
{
	zassert_is_null(alp_dac_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}
```
Assert macros in use: `zassert_is_null`, `zassert_equal`, `zassert_true`,
`zassert_ok`. All five `ZTEST_SUITE` fixture args are `NULL` — per-test state
lives on the stack; there is no setup/teardown convention to follow.

Registering a new test file takes two edits:
- `tests/zephyr/peripheral/CMakeLists.txt:12-31` — add one
  `target_sources(app PRIVATE src/<name>.c)` line.
- `tests/zephyr/peripheral/testcase.yaml` — a scenario with
  `platform_allow: [native_sim, native_sim/native/64]`, plus
  `extra_args: ["EXTRA_CONF_FILE=prj_*.conf"]` when the test needs a Kconfig
  that is not on by default.

**Test tree, by framework** — do not guess which one a directory uses:

| dir | framework | runner |
|---|---|---|
| `tests/unit/<name>/` | ztest (calls `find_package(Zephyr REQUIRED)`) | twister |
| `tests/zephyr/<name>/` | ztest | `west twister` |
| `tests/yocto/*.c` | plain-C harness, `ALP_ASSERT_TRUE` / `ALP_TEST_FAIL` from `tests/yocto/test_assert.h` — **not** Unity, **not** ztest | `ctest` |
| `tests/baremetal/` | plain-CMake, mirrors yocto | `ctest` |
| `tests/scripts/test_*.py` | pytest | `pytest tests/scripts` |
| `tests/hil/` | Python HiL runner over YAML smoke specs | `tests/hil/run_smoke.py [--validate\|--dry-run\|<real-hw>] <path>` |

**Adding a `check_*.py` gate costs four synchronised edits** (there are 63
gates today). The registry is `metadata/quality-tasks-v1.json`, schema
`metadata/schemas/quality-tasks-v1.schema.json`; entry shape:
```json
{
  "id": "agents-md-generators",
  "description": "...",
  "runner": "check-script",
  "script": "scripts/check_agents_md_generators.py",
  "gate": true,
  "profiles": ["pr", "full", "release"],
  "output": "none",
  "ci": "pr-doc-drift.yml:doc-drift"
}
```
`scripts/check_quality_registry.py` verifies the `ci` claim by grepping the
named workflow's named job for the script name — a registry entry cannot claim
CI wiring that does not exist. The four sites that must move together: the
script, the registry entry, the workflow step, and
`tests/scripts/test_check_<name>.py`.

**`scripts/test-all.sh --target dev` stage list** (`test-all.sh:258-864`):
`stage_twister`, `stage_shellcheck`, `stage_bash32_parse`, `stage_clang_format`,
`stage_metadata_validate`, `stage_alp_lock`, `stage_doc_yaml_fragments`,
`stage_public_private`, `stage_cross_platform_lint`, `stage_pytest_scripts`,
`stage_required_gate_scripts`, `stage_hil_spec_validate`,
`stage_generated_files`. Skipped under `--target dev`:
`stage_yocto_build_and_ctest`, `stage_baremetal_build` (`:1016-1017`) and
`stage_doxygen` (`:1137`). `stage_abi_strict` is `--target main` only (`:846`).

**Required PR checks**, per `docs/branching-and-merge-policy.md:292-299`:
`pr-static-analysis`, `pr-doxygen`, `pr-plain-cmake`, `pr-twister`,
`pr-metadata-validate`, `pr-generated-files`, `pr-gd32-bridge-build`, and
`pr-abi-snapshot` (informational pre-1.0, hard gate post-1.0,
`:176-181`). Everything else under `.github/workflows/pr-*.yml` is advisory on
a `dev`-targeted PR. **Confirm this against live branch-protection settings
before relying on it** — that doc lists workflow-level names while
`pr-twister.yml:37-520` posts job-level names (`twister-shard N/4` and
`twister · native_sim/native/64`), and the two have drifted before.

**Prior art for the two big patterns** — read these before writing either fix:

| issue | what it fixed |
|---|---|
| #629 | Introduced `alp_handle_op_enter`/`_leave` + `alp_handle_begin_close_blocking` for `void`-returning `close()` classes that could not take an ABI-breaking return-type change; makes `close()` block until idle. |
| #756 | (a) `alp_handle_drain_blocking()` for a two-step shutdown/destroy split; (b) the reentrant self-close guard — `alp_handle_begin_close_selfaware` + `alp_handle_cb_enter`/`_leave` + `alp_handle_take_deferred_close` — for a callback that closes its own handle on the calling thread. |
| #1114 | Removed the busy-spin drain outright: a higher-priority closer spinning never yields to a lower-priority op thread under Zephyr's preemptive scheduler. All sites repointed at the sleep-poll `alp_handle_begin_close_blocking()`. |
| #1115 | Converted static-pool `in_use` claims from check-then-set to `alp_slot_try_claim()`'s atomic compare-exchange, closing a TOCTOU where two openers win the same slot and alias handles. |

Note the two close variants are **not** interchangeable:
`alp_handle_begin_close_blocking()` drains `active_ops` on the calling thread
and **deadlocks** if called from inside the handle's own callback-invoking op.
`alp_handle_begin_close_selfaware()` detects that case via
`cb_thread`/`cb_active` and returns `ALP_HANDLE_CLOSE_DEFERRED` without
draining, leaving the op wrapper to finish the close via
`alp_handle_take_deferred_close()`.

---

## Step 0 — Unblock the tree (maintainer, blocking)

Nothing below can branch cleanly until this is resolved.

The working tree currently holds **282 staged uncommitted files** on
`fix/1510-stale-crates-oracle-doc-lines`, which is **1 commit ahead / 57
commits behind `origin/dev`**. Two consequences:

1. A hardening branch cut from here inherits 282 unrelated staged files into
   its diff — the exact mis-base failure `opening-github-prs-and-issues` §2
   exists to prevent.
2. Several audit findings cite files that may already be modified in that
   staged set, so a "fix" could collide with work already done.

**Required before Plan 1 starts:** commit or stash the 282 files, then
`git fetch origin && git merge origin/dev`. Every sub-plan below assumes a
branch cut from an up-to-date `origin/dev`.

---

## The three systemic weaknesses

The sub-plans are organised around these, not around the individual bugs.
Every sub-plan states which weakness it closes.

**W1 — Bug-class fixes were scoped to a hand-list instead of a grep.**
The atomic slot claim (#1115) has 11 un-migrated sites; the `alp_delay_us`
non-yielding contract 33; the camera #245/#246 fixes 4; the #1114 sleep-poll
drain one holdout. Each was "fixed" and each still ships the same defect
elsewhere. Closing this means a gate, not another sweep.

**W2 — Backend-to-backend parity is enforced nowhere.**
For the same portable op, the Zephyr and Linux/GD32 siblings disagree on
validation, error code, timeout convention, and returned shape — ~16 sites.
This is where the bugs sit, and it directly breaks the SoM-swap promise that
the portable API exists to make.

**W3 — Lifetime is owned per-handle only.**
Cross-handle and callback-outlives-slot lifetime is the question #629 never
asked — 7 sites, and it produces two of the three criticals.

---

## Sub-plans

| # | Plan | Issue(s) | Closes | Sites | Bench | Depends on |
|---|---|---|---|---|---|---|
| 1 | `2026-08-23-critical-memory-corruption.md` | #1618 #1619 #1620 | W3 (partly) | 3 | 3 of 3 | Step 0 |
| 2 | `2026-08-23-slot-claim-sweep.md` | #1630 | W1 | 9 + gate | no | Step 0 |
| 3 | `2026-08-23-backend-parity-conformance.md` | #1635 | W2 | ~14 + gate | no | design pass |
| 4 | `2026-08-23-cross-handle-lifetime.md` | #1644 | W3 | 4 (+3 filed separately) | yes | design pass, Plan 1 |
| 5 | `2026-08-23-unbounded-length-sweep.md` | #1645 | W2 | 6 | partly | Plan 1 |
| 6 | `2026-08-23-zephyr-errno-twin.md` | #1638 | W2 | 27 | no | Step 0 |
| 7 | `2026-08-23-watchdog-config.md` | #1637 | — | 3 defects | yes | Plan 3 (cap flag) |
| 8 | `2026-08-23-metadata-drift.md` | #1636 | — | 5 | no | Step 0 |

### The full issue slate (31 issues, #1618–#1648, opened 2026-08-23)

17 on milestone `v0.17.0`, 14 on `Backlog`; 3 carry `release-blocker`, 14 carry
`needs-silicon`.

**Criticals (`release-blocker`, `v0.17.0`):** #1618 cc3501e proxy non-handle →
ISR OOB call · #1619 i2s unbounded `memcpy` into a `k_mem_slab` block ·
#1620 GATT on-stack ctx registered across a timeout.

**High (`v0.17.0`):** #1621 34 `alp_delay_us()` busy-waits ≥ 1 ms ·
#1622 `EVK_MB_ANA`/`EVK_ARD_A0` → `ALP_E1M_ADC0` vs metadata ·
#1623 `rv3028c7_dispatch_irq` writes `0x00` to `RV3028_REG_STATUS` ·
#1624 `alp_pwm_set_duty` always `ALP_ERR_INVAL` on the GD32 bridge ·
#1625 `alp_update_log_open()` busy-spins with no sleep tick ·
#1626 unvalidated `rail_mv` → SE DC-DC setter · #1627 counter alarm survives
close · #1628 pre-v4.4 Zephyr video API in two camera backends ·
#1629 `fir_apply()` in-place reads its own output as the delay line ·
#1630 nine unlocked check-then-set slot claims · #1631 `can` `z_send` 64 B into
an 8-byte `can_frame.data` · #1632 `rpc` subscribe table dispatched unlocked ·
#1633 P65 `DEEPX_PWR_EN_REQ` IRQ armed before `k_work_init()` ·
#1634 ADC stream/filter/spectrum have no lifecycle.

**Pattern epics (`Backlog`):** #1644 close-frees-a-slot-a-callback-still-points-at
(W3) · #1635 backend parity (W2) · #1645 unbounded caller/peer length ·
#1646 fix-one-instance-never-grep-the-siblings (W1) · #1636 metadata drift ·
#1647 portable/vendor boundary leaks · #1648 config fields accepted and dropped
with `ALP_OK`.

**Features (`Backlog`):** #1637 watchdog config + the device-wide disable ·
#1638 `alp_status_from_zephyr_errno()` · #1639 UART hardware flow control ·
#1640 the hollow capability layer · #1641 41 unguarded ops-vtable calls ·
#1642 admission control asks the SoC cap table not the registry ·
#1643 no RPC peer-liveness signal.

Two audit candidates were **dropped before filing** as duplicates of #1630 —
they described the same slot-claim class with a smaller (8) and a differently
wrong (11) site count. Nine is the re-derived figure; see Plan 2.

### Plan 1 — Critical memory corruption
Three defects, three PRs, file-disjoint, no shared dependency.
- `src/backends/i2s/zephyr_drv.c:218` — caller byte count memcpy'd into a
  fixed `k_mem_slab` block. **No bench needed**; smallest of the three; ship first.
- `src/backends/gpio/cc3501e_proxy.c:131` — a non-handle passed to `z_open`,
  so `CONTAINER_OF` yields a bogus owner and the ISR calls an out-of-bounds
  function pointer. **Bench: E1M-AEN801.**
- `src/backends/ble/zephyr_drv.c:742` and `:784` — a stack ctx left registered
  with the BT host on timeout. **Bench: E1M-AEN801.**


**Bench count corrected 2 of 3 -> 3 of 3 (2026-08-23, during implementation).** The i2s fix
was expected to be verifiable on `native_sim`; it is not. Its ztest **skips** because
`alp_i2s_open()` returns NULL there — `src/i2s_dispatch.c:65` has no
`alp_backend_select_next` fall-through, so `zephyr_drv` wins selection, fails
`ALP_ERR_NOT_READY` on a NULL `_devs[0]`, and `sw_fallback` is never reached. Zephyr has no
`i2s_emul`, `vnd_i2s`'s configure returns `-ENOTSUP`, and i2s `sw_fallback`'s write returns
`ALP_ERR_NOSUPPORT` by design. A green native_sim run does not verify this fix.

### Plan 2 — Slot-claim sweep
Migrate the remaining pools onto `alp_slot_try_claim()`
(`src/common/alp_slot_claim.h:39`), then add a `scripts/check_*.py` gate so the
next one cannot be written un-migrated. The gate is the deliverable; the sweep
is the precondition. Closing W1 without the gate just resets the clock.

**Scope corrected from the audit's 11 to 9 by a fresh grep on 2026-08-23.**
Two of the audit's sites are false positives — their check-then-set is already
serialised by a held mutex, which is correct:
- `src/zephyr/handles.c:29` — the whole loop runs inside
  `k_mutex_lock(&kind##_lock, K_FOREVER)`.
- `src/yocto/peripheral_gpio.c:131` — inside `pthread_mutex_lock(&g_irq.mu)`.

Do **not** "fix" either; converting a mutex-serialised pool to a CAS is a
no-op at best. The planned gate must whitelist both, or it will re-report them
forever.

### Plan 3 — Backend parity + conformance suite
**The highest-leverage item in the whole audit**, and both reports converged on
it independently. 14 confirmed divergences across 11 classes, in three families:
one sibling validates and the other does not (6), divergent status for an
identical condition (5), and timeout-convention drift (3).

**Correction to a first reading:** this is new machinery inside the existing
suite, not an extension of it. `tests/zephyr/conformance/` is **two mutually
exclusive app images** by deliberate design — `CMakeLists.txt` is a hard
`if(CONFIG_ALP_SDK_TESTING)` / `else()` split, because `main.c`'s 16-row
`conf_classes[]` table assumes the real/emulated backend (gpio Case B expects
`alp_gpio_open(99)` to fail) while the seven `behavior_*.c` files drive
priority-255 wildcard doubles that must open *any* instance. Neither image runs
one call against two backends.

**The design decision the plan turns on:** there is no way to make
`alp_<class>_open()` use a chosen backend — the dispatchers call
`alp_backend_select()` and take the winner (`src/uart_dispatch.c:63`), and no
pin/override mechanism exists anywhere in the tree. Adding a test-only seam to
production dispatch was rejected; the harness instead enumerates the registry
with `alp_backend_select_next()` (`include/alp/backend.h:210`) and drives each
backend's ops table directly. That works — `uart/zephyr_drv.c` and
`uart/sw_fallback.c` are both linked on native_sim
(`zephyr/CMakeLists.txt:1442`, `:1444`) — at the cost that dispatcher-internal
divergences (`src/storage_dispatch.c:157` vs `:178`) stay invisible to it and
are fixed directly.

The harness lands **red** on purpose: written after the fixes it would be tuned
to pass rather than tuned to detect.

### Plan 4 — Cross-handle lifetime
Needs a design pass before any code: the invariant does not exist yet. #629
asked "can an in-flight *operation* race close()?" and answered it. It never
asked the adjacent question — a registered callback is not an in-flight op, so
draining `active_ops` does not wait for it and `close()` frees a slot it still
points at.

The rule is written down in exactly one place, and correctly:
`src/i2c_regfile.c:218-228` — **drain, then stop the callback source, then
release the slot**, in that order. Task 1's deliverable is moving that sentence
into `src/common/alp_slot_claim.h` where a dispatcher author will actually meet
it, plus a checklist row in `writing-race-safe-dispatch-handlers`.

**Scope corrected to 4 sites, not 7.** The other three instances of this class
are already filed on their own and are referenced, not restated: #1627 (counter
alarm survives close), #1620 (GATT on-stack ctx, a release-blocker in Plan 1),
#1634 (ADC stream handles have no lifecycle at all). In scope here:

- `src/ble_dispatch.c` — radio close never walks `_conn_pool`. Verified:
  `_free_conn` (`:138`) is reached from exactly two places, `:536`
  (connect-failure) and `:570` (`alp_ble_disconnect`). So closing a radio with
  connections open leaves every `c->state.radio` (`:533`) dangling **and**
  strands the conn slots — after `CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES` cycles
  `alp_ble_connect()` returns `ALP_ERR_NOMEM` permanently. The leak is the half
  a customer hits first, and it looks like a resource leak rather than a
  lifetime bug, which is why it survived.
- `src/gui_lvgl.c:128` — a raw `alp_display_t *` parked in LVGL's user-data with
  no detach; `_flush_cb` (`:96`) reads it back on every blit. The file's
  `:145-146` comment justifies this, but that reasoning was written about the
  frame buffer (genuinely process-lifetime) and then applied to the handle
  pointer (not). Note there are **two** `alp_gui_lvgl_attach` definitions
  (`:107`, `:157`) — an LVGL-version `#if` pair; both arms need the detach.
- `src/backends/mproc/zephyr_drv.c:443` — `z_mbox_close` disables the channel
  but leaves the registration live with a freed `be` as `user_data`. Defence in
  depth, not a demonstrated crash.
- `src/zephyr/handles.c:29` — re-owns a slot under a possibly-in-flight op. The
  claim itself is mutex-correct (Plan 2 allowlists it); this is the separate
  half. **Task 4b's first step is to establish reachability, and closing the
  item with a comment is a valid outcome.**

### Plan 5 — Unbounded length sweep
Six sites (the two worst, #1619 i2s and #1631 can, are filed separately as Plan
1 work). **Correcting the audit's own framing before it misleads a reviewer:**
it grouped these under "length reaches a memcpy or a DMA master with no bound",
which reads as six memory-corruption bugs. Only one of the six is memory-unsafe,
and it is unreachable in a default build. The six are three distinct shapes:

- **Unvalidated caller geometry (2, JPEG).** `src/jpeg_dispatch.c:89-91` checks
  only non-NULL and non-zero width/height. A `y_stride` of 0 aliases every row to
  row 0 and emits a structurally valid JPEG of the wrong image with `ALP_OK`; the
  `max_width`/`max_height` the backends advertise (16384 at
  `src/backends/jpeg/sw_baseline.c:36-37`) are never enforced. At
  `src/backends/jpeg/alif_hantro.c:264` the same geometry reaches the JPEG **AXI
  master**, so an under-sized stride is hardware reading memory the caller did
  not intend to expose.
- **Peer length silently truncated (3) — the ones a customer actually hits.**
  `src/backends/rpc/yocto_uio_drv.c:757` clips a peer frame over
  `ALP_RPC_TX_FRAME_MAX` (1024) and dispatches it as complete;
  `src/backends/rpc/yocto_drv.c:366` lets `read()` truncate and leaves the tail
  queued, misaligning every later frame. Worst is
  `src/backends/mqtt/zephyr_drv.c:260`: the `MIN` bounds the copy correctly (the
  audit's memory-safety claim was rightly refuted) but the remainder is never
  drained, so `remaining_payload` stays > 0, every later `mqtt_input()` returns
  `-EBUSY`, and the connection stops delivering **permanently** — after the
  message was already PUBACKed at `:268-271`, so the broker will not resend. One
  broker-controlled length bricks the link.
- **Multi-input DMA over PCIe (1, latent).** `src/yocto/inference_deepx.cpp:279`
  passes `input_bufs[0].data()` where the struct's own comment (`:93-97`) says
  `Run()` needs one pointer to the **concatenated** inputs — but `input_bufs` is
  `std::vector<std::vector<uint8_t>>`, separate allocations. For any >1-input
  model dx_rt reads `sum(size_in_bytes)` past input 0 and DMAs unrelated heap to
  the DX-M1. Gated `ALP_SDK_USE_DEEPX_DXM1` (`src/yocto/CMakeLists.txt:67`),
  default OFF, BENCH-UNVERIFIED. **Must be fixed before that gate is flipped for
  V2M bring-up**, and the plan takes the refuse-at-open fix rather than writing a
  concatenation that would ship untested.

Also records the **zero cache maintenance anywhere in `src/`** gap as a note
rather than fixing it — adding barriers to a DMA path without measuring on real
silicon trades a visible gap for an intermittent one.

### Plan 6 — Zephyr errno twin
Ship the negative-errno counterpart to `alp_status_from_posix_errno` and delete
the hand-copied switches. `src/common/alp_errno.h:19-21` already documents this
as the missing piece, and `src/backends/can/yocto_drv.c:178-190` is the working
precedent for the override form — and, it turns out, the **only** consumer of
`alp_status_from_posix_errno_ex` anywhere in the tree.

**Corrected by a full inventory on 2026-08-23: 27 sites, not 27-28.** Two other
corrections that change how this must be done:

- **It is not a pure refactor.** The POSIX baseline maps `EAGAIN` to
  `ALP_ERR_BUSY` and **zero of the 27 Zephyr sites agree** — 16 map `-EAGAIN` to
  `ALP_ERR_TIMEOUT`, and 11 have no arm at all so it falls to `ALP_ERR_IO`.
  Delegating naively would silently rewrite 16 drivers. The plan decides the
  question explicitly (the Zephyr twin maps `-EAGAIN` to `ALP_ERR_TIMEOUT`, a
  documented cross-domain divergence) and migrates in three behaviour-classified
  batches so the 11 sites that *do* change arrive as their own reviewable PR.
- **One real documented-contract violation, not many.** The six classes that
  cannot return `ALP_ERR_TIMEOUT` do not document it either, so there is no
  breach there. The genuine one is `include/alp/storage.h:142-152`, which
  promises `alp_storage_open()` returns `ALP_ERR_TIMEOUT` "translated from the
  underlying flash-area / littlefs error" while
  `src/backends/storage/zephyr_littlefs.c:80` has no arm that can produce one.
  Its `zephyr_flash.c:36` sibling can. One header, one of two backends,
  contradicting itself.

No ABI impact: `src/common/alp_errno.h` is internal-only — `CMakeLists.txt:392-393`
installs only `include/alp`, no public header includes it, and `docs/abi/` has no
`alp_errno` reference.

### Plan 7 — Watchdog config
Three defects in one struct plus one live safety bug. Ship the live bug
**first and alone**: `src/wdt_dispatch.c:53-89` never checks `cfg->wdt_id`
against a live handle and `src/backends/wdt/zephyr_drv.c:86-91` calls
`wdt_disable(dev)` on the whole device, so one subsystem's close silently
removes another's protection. Pure dispatcher code, no backend risk.

### Plan 8 — Metadata drift
Four sites where a hardware fact drifted back out of `metadata/` into a
hand-written header. Every fix is a generator or YAML change; hand-editing a
generated header is the one thing this plan must never do.

The worst is the whole E1M-X EVK I2C block. `grep -n "i2c_devices"
metadata/boards/e1m-x-evk.yaml` returns **nothing** — the five INA236 addresses,
their shunts and their max currents exist only in
`include/alp/boards/alp_e1m_x_evk.h:80-100`, while the sibling E1M carrier's
identical data is generated into `alp_e1m_evk_routes.h:135-140` (lifted under
#515). `XEVK_INA236_SHUNT_3V3_OHMS 0.020f` is not an identifier but a scaling
factor — every +3V3 current reading goes through it, so a respin desyncs it into
plausible-but-wrong numbers rather than a build break.

A different failure mode at the third site, and the audit's count was off by
one. `ALP_SOC_NPU_ARENA_SRAM_KIB` is `0` on **nine** real SoCs
(`include/alp/soc_caps.h:66,107,148,189,230,271,311,351,391`) — not ten;
`:430` is `UINT16_MAX`, the `CONFIG_ALP_SOC_NONE` permissive fallback emitted by
`scripts/gen_soc_caps.py:559`, and is correct. The metadata field exists and is
wired through the generator correctly; it is simply zero, and
`src/backends/inference/alp_model_select.c:85` reads zero as accept-anything:

```c
	return e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib;
```

So the `ALP_ERR_NO_FIT` envelope check `include/alp/inference.h:232-238`
promises is dead on every shipped SoM, and an oversize Ethos-U blob faults at
invoke instead of failing cleanly at load. The plan keeps `0 == unbounded` and
makes the **generator** refuse to emit a zero arena for an SoC that declares an
NPU — so an unpopulated field can never again silently disable the check.

`include/alp/cap.h:58` was listed by the audit without detail and is **not
verified**; Task 4's first step is to characterise it, and closing it as "the
audit was wrong here" is an explicitly valid outcome.


---

## Sequencing

1. **Step 0** — maintainer resolves the 282 staged files. Blocking.
2. **Plan 1** — all three criticals. Start the i2s one immediately; the two
   bench-gated ones can be written in parallel and batched into one
   E1M-AEN801 session.
3. **Plan 2 and Plan 6 in parallel** — file-disjoint (`src/backends/*/` pools
   vs `src/backends/*/zephyr_drv.c` errno switches, with an overlap only in
   which files are touched, not which lines). Both are mechanical and both
   remove a per-vendor cost permanently.
4. **Plan 3 design pass — start immediately, in parallel with everything.**
   It gates Plan 7's capability flag and it is the only item whose benefit
   scales linearly with vendor count.
5. **Plan 4 design pass**, then its sites. Depends on Plan 1 landing first so
   the two criticals are not being edited from two directions.
6. **Plan 5, Plan 7, Plan 8** — parallel, file-disjoint. Plan 7's live
   dispatcher fix can jump the queue at any time; it is independent.

Plans 1, 4, and 7 need bench time on real silicon. Batch their verification
into one E1M-AEN801 session and one E1M-V2N101 session rather than reserving
the bench six times.

---

## What is deliberately NOT in this campaign

- The 78 medium and 15 low findings. They are folded into the pattern epics on
  GitHub; individually they do not justify a PR each.
- Bus transfer deadlines (`timeout_ms` on the ten core-bus ops). Real gap, but
  70+ files and it **changes the close-drain class** — per #629 a `timeout_ms`
  blocking op requires `alp_handle_begin_close_blocking`, so adding deadlines
  without changing the dispatchers' close paths converts a use-after-free class
  into a deadlock class. Sequence per class over several releases, after Plan 4.
- DMA/async variants for I2C/SPI/UART. Downstream of the deadline work.
- Anything the feature-gap review killed: the GD32 DSP backend (already exists
  twice over), `include/alp/fw_slot.h` A/B commit-confirm (contradicts
  `SB_CONFIG_MCUBOOT_MODE_SINGLE_APP=y` and would be a second writer of the
  `bootcount` vars mender-client owns), `ALP-Rxxx` runtime codes (already
  `alp_status_name()`), and an `alp_log` sink (downstream of
  `alp_last_error_detail()`).
