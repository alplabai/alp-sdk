# Watchdog Config — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue: #1637** (`enhancement`, `area:drivers`, `area:portability`, `needs-silicon`, milestone `Backlog`)

**Goal:** Close four defects in one config struct — a live safety bug where one subsystem's `close()` disarms another's watchdog, an inert `ALP_WDT_INTERRUPT_ONLY` mode, no windowed feed, and a watchdog that resets the SoC during deep sleep.

**Architecture:** Four separable changes, and **the order is not negotiable.** Task 1 is the live defect: pure dispatcher code, no backend risk, no new API, ships alone and immediately. Tasks 2-4 extend `alp_wdt_config_t`, which is one ABI change best made once — but each is reviewable on its own, and Task 2 is the only one that needs a callback trampoline.

**Tech Stack:** C (clang-format 22.x, tabs), Zephyr, Yocto/Linux, ztest, twister on `native_sim/native/64`, J-Link SWD on E1M-AEN801.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections first.

## Global Constraints

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 280 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` green before `gh pr create`.
- clang-format **22.x** on every changed `.c`/`.h` including test files.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result.
- **`alp_wdt_config_t` is public** (`include/alp/wdt.h`). Every task from 2 onward is an ABI change — regenerate the ABI snapshot or `check · generated files in sync` goes red.
- No AI attribution anywhere.
- **Collides with Plan 3** (`2026-08-23-backend-parity-conformance.md`, #1635) Task 2 Step 4, which fixes the leaked timeout channel in this same `z_open`. Whichever lands second rebases. Say so in both PRs.

---

## The four defects, measured

### (d) The live one — one subsystem's close disarms everyone's watchdog

`alp_wdt_open()` validates the config and picks a backend, but **never checks `cfg->wdt_id` against a live handle**:

```c
/* src/wdt_dispatch.c:54-62 */
alp_wdt_t *alp_wdt_open(const alp_wdt_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->timeout_ms == 0u) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("wdt", ALP_SOC_REF_STR);
```

and `close` disables the **whole device**, not the handle's channel:

```c
/* src/backends/wdt/zephyr_drv.c:89-93 */
static void z_close(alp_wdt_backend_state_t *st)
{
	const struct device *dev = (const struct device *)st->dev;
	/* Most M-class watchdogs don't allow disable; ignore error. */
	(void)wdt_disable(dev);
}
```

So two subsystems can each open `ALP_E1M_WDT0`, and when the first closes, **the second's protection silently disappears** — `(void)`-cast, no error on any path, nothing in either caller's return value. A device that believes it is watchdog-protected and is not is worse than one with no watchdog at all, because nobody is looking.

This is the whole reason Task 1 ships first and alone.

### (a) `ALP_WDT_INTERRUPT_ONLY` is inert

The header offers the mode:

```c
/* include/alp/wdt.h:52-57 */
typedef enum {
	ALP_WDT_RESET_SOC      = 0, /**< Full SoC reset (default; safest). */
	ALP_WDT_RESET_CPU      = 1, /**< Core reset only — peripherals keep state. */
	ALP_WDT_INTERRUPT_ONLY = 2  /**< Generate an IRQ; no reset. */
} alp_wdt_action_t;
```

and the backend selects `WDT_FLAG_RESET_NONE` with a hardcoded `NULL` callback:

```c
/* src/backends/wdt/zephyr_drv.c:60-67 */
	struct wdt_timeout_cfg zcfg = {
		.window   = { .min = 0u, .max = cfg->timeout_ms },
		.callback = NULL,
		.flags    = (cfg->on_timeout == ALP_WDT_INTERRUPT_ONLY)
		                ? WDT_FLAG_RESET_NONE
		                : (cfg->on_timeout == ALP_WDT_RESET_CPU ? WDT_FLAG_RESET_CPU_CORE
		                                                        : WDT_FLAG_RESET_SOC),
	};
```

`WDT_FLAG_RESET_NONE` + `.callback = NULL` means the timeout fires into **nothing**: no reset, and no notification. A customer who picks this mode — to persist a crash breadcrumb, flush a log, or park an actuator before the reset — ships a watchdog they believe is armed that does absolutely nothing.

**It is worse on Linux, in the opposite direction.** The Yocto backend documents that it silently ignores the field:

```
/* src/backends/wdt/yocto_drv.c:16-20 */
 * On the alp_wdt_action_t mapping: the Linux watchdog ABI exposes NO
 * knob for reset-scope (SoC vs core) or interrupt-only mode -- that is
 * fixed by the kernel driver + device-tree.  cfg.on_timeout is
 * therefore informational only on Linux; we honour timeout_ms (the one
 * field the ABI lets us set) and leave the action to the platform.
```

So the same `ALP_WDT_INTERRUPT_ONLY` that does nothing on Zephyr **resets the SoC** on Linux. One enum, two silent and opposite divergences from what it says.

### (b) No windowed mode

`.window.min` is hardcoded `0u` (`zephyr_drv.c:61`). Functional-safety claims generally require a *minimum* feed interval too — a task stuck in a tight loop feeding early is as much a fault as one that never feeds.

`grep -rn "window_min"` returns exactly one hit in the whole tree: `docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md:125`, which already ratified `window_min_ms` as `PORTABLE (deferred)` and reserved `ALP_INSTANCE_CAP_HW_TIMEOUT` for it at `:111`. This task is cashing a decision already made, not making a new one.

### (c) The watchdog resets the SoC during deep sleep

```c
/* src/backends/wdt/zephyr_drv.c:72 */
	int err        = wdt_setup(dev, 0);
```

The options mask is literally zero — neither `WDT_OPT_PAUSE_IN_SLEEP` nor `WDT_OPT_PAUSE_HALTED_BY_DBG`. So an armed 5 s watchdog resets the SoC in the middle of:

```c
alp_power_request_sleep(p, ALP_POWER_MODE_DEEP_SLEEP, 30000, &wake);
```

and it resets the debugger's target the moment you halt at a breakpoint.

**One plain warning, because this is the item most likely to be got wrong:** `pause_in_sleep = true` means the watchdog does **not** protect against a hang inside the sleep path itself. Never default it true; make the customer ask for it.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/wdt_dispatch.c:54-92` | Modify: reject a second open of a live `wdt_id` | 1 |
| `tests/zephyr/peripheral/src/wdt.c` | Modify: ztest for the exclusivity rejection | 1 |
| `include/alp/wdt.h:62-66`, `:83-84` | Modify: config struct + `ALP_WDT_CONFIG_DEFAULT` | 2, 3, 4 |
| `src/backends/wdt/zephyr_drv.c` | Modify: trampoline, `.callback`, `.window.min`, `wdt_setup` options | 2, 3, 4 |
| `src/backends/wdt/yocto_drv.c` | Modify: stop silently accepting what it cannot honour | 2, 3 |
| `src/backends/wdt/sw_fallback.c` | Modify: keep `@par Cost:` / `@par Performance:` tags valid | 2, 3, 4 |
| `src/backends/wdt/wdt_ops.h` | Read only — already stores `cfg` and `channel_id` | 2 |
| `include/alp/cap_instance.h` | Modify: set `ALP_INSTANCE_CAP_HW_TIMEOUT` when the window is honoured | 3 |

---

## Task 1: One handle per watchdog instance

**Files:** `src/wdt_dispatch.c`, `tests/zephyr/peripheral/src/wdt.c`.

**Interfaces:** no API change. No new symbol. No ABI regen.

**Ship this alone, first, before anything else in this plan.** It is the only live safety defect, it touches one file, and it needs no bench time.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/1637-wdt-instance-exclusivity origin/dev
```

- [ ] **Step 2: Write the failing test**

Append to `tests/zephyr/peripheral/src/wdt.c`:

```c
ZTEST(alp_peripheral, test_wdt_second_open_of_same_id_is_refused)
{
	alp_wdt_config_t cfg = ALP_WDT_CONFIG_DEFAULT(0u);

	alp_wdt_t *first = alp_wdt_open(&cfg);
	if (first == NULL) {
		ztest_test_skip(); /* no watchdog backend on this platform */
	}

	/* A second handle on the SAME instance must be refused: close() disables
	 * the whole device, so two owners means one subsystem's close silently
	 * removes the other's protection. */
	alp_wdt_t *second = alp_wdt_open(&cfg);
	zassert_is_null(second, "a second open of a live wdt_id must be refused");
	zassert_equal(alp_last_error(), ALP_ERR_BUSY,
	              "the refusal must report ALP_ERR_BUSY, not a generic failure");

	alp_wdt_close(first);

	/* After the owner closes, the instance must be claimable again. */
	alp_wdt_t *third = alp_wdt_open(&cfg);
	zassert_not_null(third, "the instance must be reusable after close");
	alp_wdt_close(third);
}
```

**Check `alp_wdt_close`'s exact name and whether `ALP_WDT_CONFIG_DEFAULT` needs an `ALP_E1M_WDT0` rather than a bare `0u`** before running — `include/alp/wdt.h:64` says `wdt_id` is a "Form-factor WDT instance ID: ALP_E1M_WDT0..1 or ALP_E1M_X_WDT0..1", so a raw literal may not be the intended spelling even if it compiles.

- [ ] **Step 3: Run it and confirm it FAILS**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

Expected: `second` is non-NULL — two live handles on one watchdog.

- [ ] **Step 4: Add the exclusivity check**

In `alp_wdt_open`, after the `cfg`/backend validation and **before** `_alloc()`, scan the pool for a live handle already holding `cfg->wdt_id`.

**The mechanics are the part that is easy to get wrong.** Claiming is not atomic with the scan, so two concurrent opens of the same `wdt_id` can both pass a single pre-scan. The scan must therefore run **twice**: once before `_alloc()` to reject the common case cheaply, and again after `_alloc()` has claimed a slot — the loser then `_free()`s its slot and reports `ALP_ERR_BUSY`.

Read `in_use` with `__ATOMIC_ACQUIRE`, and read the handle's `wdt_id` only from a slot whose `in_use` reads true.

`src/ble_dispatch.c:153-190` (issue #1118) is the reference for the CAS/lifetime mechanics — **but not for the return policy.** That site is a refcounted *join* of a singleton: a second opener there succeeds and shares. The watchdog wants the opposite, exclusivity. Copy the atomics, not the semantics.

Where is `wdt_id` readable from a live handle? `src/backends/wdt/wdt_ops.h:27-28` shows `alp_wdt_backend_state_t` already carries both:

```c
	int                  channel_id; /* wdt_install_timeout return code */
	alp_wdt_config_t     cfg;
```

so `h->state.cfg.wdt_id` is available with no new plumbing. Confirm that field path against the dispatcher's own `struct alp_wdt` before using it.

- [ ] **Step 5: Run the test and confirm it PASSES**

- [ ] **Step 6: Format, gate, commit**

```bash
clang-format -i src/wdt_dispatch.c tests/zephyr/peripheral/src/wdt.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git add src/wdt_dispatch.c tests/zephyr/peripheral/src/wdt.c
git commit -m "fix(wdt): refuse a second open of a live watchdog instance

alp_wdt_open never compared cfg->wdt_id against a live handle, and
z_close calls wdt_disable(dev) on the whole device rather than the handle's
channel -- with the result (void)-cast away. Two subsystems could each open
ALP_E1M_WDT0, and the first to close silently removed the second's protection
with no error on any path.

A device that believes it is watchdog-protected and is not is worse than one
with no watchdog, because nobody is looking. Second open now returns NULL with
ALP_ERR_BUSY."
```

---

## Task 2: Make `ALP_WDT_INTERRUPT_ONLY` do something

**Files:** `include/alp/wdt.h`, `src/backends/wdt/zephyr_drv.c`, `src/backends/wdt/yocto_drv.c`, `src/backends/wdt/sw_fallback.c`.

**Interfaces:**
- Produces: `alp_wdt_expiry_cb_t`, plus `on_expire` and `user` on `alp_wdt_config_t`. Tasks 3 and 4 extend the same struct — coordinate so the ABI snapshot is regenerated once per PR, not once per field.

**The callback must be carried in the config, not set afterwards.** Zephyr installs it inside `wdt_install_timeout()`, which `z_open` calls at `zephyr_drv.c:69` — a `set_callback()` after open would have nothing to attach to. This mirrors `alp_i2c_target_config_t`, which carries `on_write`/`on_read`/`on_stop` + `user` in the config for exactly the same reason (`include/alp/peripheral.h:519-526`).

- [ ] **Step 1: Extend the public config**

```c
/** Watchdog expiry callback.  Runs in ISR context. */
typedef void (*alp_wdt_expiry_cb_t)(alp_wdt_t *wdt, void *user);

typedef struct {
	uint32_t            wdt_id;     /**< Form-factor WDT instance ID: ALP_E1M_WDT0..1 or ALP_E1M_X_WDT0..1. */
	uint32_t            timeout_ms; /**< Feed deadline in milliseconds; must be non-zero. */
	alp_wdt_action_t    on_timeout; /**< Action when the deadline is missed. */
	alp_wdt_expiry_cb_t on_expire;  /**< ISR context.  REQUIRED when
	                                 *   on_timeout == ALP_WDT_INTERRUPT_ONLY. */
	void               *user;       /**< Forwarded to on_expire. */
} alp_wdt_config_t;
```

Document on `on_expire` that it runs in ISR context and must not block, allocate, or take a mutex. Update `ALP_WDT_CONFIG_DEFAULT` at `include/alp/wdt.h:83-84` so the new fields are explicitly zeroed rather than left to the compound literal's implicit init — the macro's doc comment already explains why zero-init is not valid for this struct, and the same care applies.

- [ ] **Step 2: Reject the combination that used to silently do nothing**

In `alp_wdt_open`, reject `on_timeout == ALP_WDT_INTERRUPT_ONLY && on_expire == NULL` with `ALP_ERR_INVAL`, mirroring `alp_i2c_target_open`'s NULL-callback rejection (`include/alp/peripheral.h:562-566`). A customer who asks for interrupt-only and supplies no handler is asking for a watchdog that does nothing; say so at open instead of at 3 a.m. in the field.

- [ ] **Step 3: Build the trampoline — this is not two struct assignments**

Zephyr's `wdt_callback_t` is `void (*)(const struct device *dev, int channel_id)`. **It carries no user cookie**, unlike Zephyr's RTC and counter callbacks. So the backend needs a static `(dev, channel_id) -> alp_wdt_backend_state_t *` lookup table sized `ARRAY_SIZE(_devs) × channels`, plus one shared C trampoline that resolves the handle and calls `cfg.on_expire(wdt, cfg.user)`.

`src/backends/wdt/wdt_ops.h:27-28` already stores `channel_id` and `cfg` in the state, so the callback and its cookie are reachable once the trampoline can find the state. Budget the trampoline table — plans that said "two struct assignments" were wrong.

Then set `.callback` in `zcfg` (`zephyr_drv.c:62`) instead of the hardcoded `NULL`.

- [ ] **Step 4: Stop the Linux backend lying**

`src/backends/wdt/yocto_drv.c:16-20` documents `cfg.on_timeout` as "informational only on Linux" and silently accepts `ALP_WDT_INTERRUPT_ONLY` while the SoC resets anyway. That is a documented lie, not a documented limitation.

Make the Yocto backend **reject** `on_timeout == ALP_WDT_INTERRUPT_ONLY` at open with `ALP_ERR_NOSUPPORT`, and reject a non-NULL `on_expire` the same way — the Linux watchdog ABI has no pre-timeout hook. Fold this into the same slice or the header keeps lying on Linux.

- [ ] **Step 5: Test what can be tested, and say what cannot**

A ztest can assert the `ALP_ERR_INVAL` rejection from Step 2 and the `ALP_ERR_NOSUPPORT` from Step 4. It **cannot** prove the IRQ arrives — `native_sim` has no real watchdog expiry. Write the reachable assertions, and state plainly in the PR that the callback firing is proven only by Step 7's bench log.

- [ ] **Step 6: Format, regenerate ABI, gate, commit**

```bash
clang-format -i include/alp/wdt.h src/backends/wdt/*.c tests/zephyr/peripheral/src/wdt.c
# New public typedef + struct fields = ABI change. See regenerating-generated-files
# for the exact abi_snapshot.py invocation; do not guess the arguments.
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "feat(wdt): give ALP_WDT_INTERRUPT_ONLY an expiry callback

The mode selected WDT_FLAG_RESET_NONE with a hardcoded NULL callback, so the
timeout fired into nothing -- no reset AND no notification. A customer who chose
it to persist a crash breadcrumb shipped a watchdog that did nothing.

On Linux it failed the opposite way: yocto_drv documents cfg.on_timeout as
informational only and the SoC resets anyway. One enum, two silent and opposite
divergences from what it says. Linux now refuses the mode instead.

Zephyr's wdt_callback_t carries no user cookie, so the backend gains a
(dev, channel_id) -> state trampoline table."
```

- [ ] **Step 7: Bench — E1M-AEN801 and the V2N CM33 (BLOCKING)**

`native_sim` cannot prove an ISR fires on an M55. Arm a watchdog with `ALP_WDT_INTERRUPT_ONLY` and a real `on_expire`, deliberately miss a feed, and confirm the callback runs with the right handle and cookie. Repeat on the V2N CM33. Capture both serial logs into the PR.

The callback runs in ISR context and can race `alp_wdt_close` — apply the operation-versus-close rules from `alp-lab:writing-race-safe-dispatch-handlers` (the #629 class) and, if the close path needs to guarantee no callback fires afterwards, the cross-handle-lifetime invariant from Plan 4 (#1644) applies here too.

---

## Task 3: Windowed feed

**Files:** `include/alp/wdt.h`, `src/backends/wdt/zephyr_drv.c`, `src/backends/wdt/yocto_drv.c`, `include/alp/cap_instance.h`.

- [ ] **Step 1: Add the field**

```c
	uint32_t window_min_ms; /**< Earliest legal feed; 0 = no lower bound. */
```

placed next to `timeout_ms`, which is the window's upper bound. Document that a feed before `window_min_ms` is itself a fault on hardware that supports it.

- [ ] **Step 2: Wire it in the Zephyr backend**

Replace the hardcoded `.window = { .min = 0u, ... }` at `zephyr_drv.c:61` with `cfg->window_min_ms`.

- [ ] **Step 3: Refuse it where it cannot be honoured**

`window_min_ms != 0` must return `ALP_ERR_NOSUPPORT` from `src/backends/wdt/yocto_drv.c` — the Linux watchdog ABI has no window knob. Same discipline as Task 2 Step 4: refuse rather than accept-and-ignore.

- [ ] **Step 4: Set the capability flag**

`ALP_INSTANCE_CAP_HW_TIMEOUT` is already reserved for this by `docs/superpowers/specs/2026-05-22-simple-peripherals-slice4a-design.md:111`. Set it in `caps_out` when the backend honours the window.

**This is the one dependency on Plan 3** (#1635, the capability layer work). If `cap_instance.h`'s flag semantics are still being settled there, land Steps 1-3 and add the flag in a follow-up rather than guessing at a contract another plan is defining.

- [ ] **Step 5: Format, regenerate ABI, gate, commit**

```bash
git commit -am "feat(wdt): windowed feed via window_min_ms

.window.min was hardcoded to 0, so a task stuck in a tight loop feeding early
was indistinguishable from a healthy one -- which functional-safety claims
generally require detecting. The field was already ratified as PORTABLE
(deferred) in the slice4a design doc, with ALP_INSTANCE_CAP_HW_TIMEOUT reserved
for it. Linux refuses a non-zero window: the ABI has no such knob."
```

---

## Task 4: Sleep and debug pause

**Files:** `include/alp/wdt.h`, `src/backends/wdt/zephyr_drv.c`.

- [ ] **Step 1: Add the two flags**

```c
	bool pause_in_sleep; /**< Halt the counter in low-power modes.  See the warning below. */
	bool pause_on_debug; /**< Halt the counter while halted by a debugger. */
```

**Document the hazard on `pause_in_sleep` in the header itself, not just the commit:** setting it true means the watchdog does **not** protect against a hang inside the sleep path. Default false, and never flip that default.

- [ ] **Step 2: Build the options mask**

Replace `wdt_setup(dev, 0)` at `zephyr_drv.c:72` with a mask assembled from the two flags — `WDT_OPT_PAUSE_IN_SLEEP` and `WDT_OPT_PAUSE_HALTED_BY_DBG`.

- [ ] **Step 3: Cross-reference the power API**

Add a `@note` to `include/alp/power.h` beside `alp_power_request_sleep` pointing at `pause_in_sleep`: an armed watchdog with a `timeout_ms` shorter than the requested sleep resets the SoC mid-sleep unless the flag is set. Someone reading the power API is exactly the person who needs to know.

- [ ] **Step 4: Bench — measure, do not read the datasheet (BLOCKING)**

Vendor watchdog documentation is unusually unreliable about whether the counter truly halts in low-power modes. On E1M-AEN801: arm a 5 s watchdog with `pause_in_sleep = true`, call `alp_power_request_sleep(p, ALP_POWER_MODE_DEEP_SLEEP, 30000, &wake)`, and confirm the SoC wakes on the RTC rather than resetting at 5 s. Then repeat with `pause_in_sleep = false` and confirm it *does* reset — a flag that appears to work because the counter never ran is not a passing test.

- [ ] **Step 5: Format, regenerate ABI, gate, commit**

```bash
git commit -am "feat(wdt): pause_in_sleep / pause_on_debug

wdt_setup() was called with an options mask of literally zero, so neither
WDT_OPT_PAUSE_IN_SLEEP nor WDT_OPT_PAUSE_HALTED_BY_DBG was ever set: an armed
5 s watchdog reset the SoC in the middle of a 30 s
alp_power_request_sleep(ALP_POWER_MODE_DEEP_SLEEP), and reset the target on
every debugger halt.

pause_in_sleep defaults false and is documented as NOT protecting against a hang
inside the sleep path itself."
```

---

## Opening the PRs

Four PRs, all `--base dev`, strictly in task order.

- Task 1: `Refs #1637.` Labels `bug`, `area:drivers`. **No ABI regen, no bench.** This is the one to merge fastest.
- Task 2: `Refs #1637.` Labels `enhancement`, `area:drivers`, `needs-silicon`. ABI regen.
- Task 3: `Refs #1637.` Labels `enhancement`, `area:drivers`, `area:portability`. ABI regen.
- Task 4: `Closes #1637.` Labels `enhancement`, `area:drivers`, `needs-silicon`. ABI regen.

**Blast radius — 18 files** once Tasks 2-4 land: `include/alp/wdt.h`, `include/alp/power.h`, `include/alp/cap_instance.h`, `src/wdt_dispatch.c`, `src/backends/wdt/{wdt_ops.h,zephyr_drv.c,yocto_drv.c,sw_fallback.c}` (its `@par Cost:` / `@par Performance:` tags must stay valid — a gate checks them), `tests/zephyr/peripheral/src/{wdt.c,config_defaults.c}`, `tests/unit/wdt_registry/src/test_wdt_registry.c`, `tests/yocto/peripheral_wdt.c`, `tests/zephyr/conformance/src/main.c`, `tests/hil/_common/wdt-feed.yaml`, `examples/power-timing/wdt-feed/{src/main.c,README.md,boards/native_sim_native_64.overlay}`, `examples/aen/aen-wdt-feed/{src/main.c,testcase.yaml}`, `docs/abi/` snapshot, `tests/fixtures/stub-symbol-matrix/symbols.json`.

**Bench:** Tasks 2 and 4 both need E1M-AEN801, and Task 2 also wants the V2N CM33. Batch them into one reservation with Plan 1's #1618/#1620 and Plan 4's Task 2 — five bench items, one session.
