# Cross-Handle Lifetime — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue: #1644** (`bug`, `area:drivers`, `area:portability`, milestone `Backlog`)

**Goal:** Make "no callback fires after this" a dispatcher-wide invariant instead of one file's comment, and close the four sites where `close()` frees a slot that something else still points at.

**Architecture:** #629 asked "can an in-flight *operation* race `close()`?" and answered it with `alp_handle_op_enter` / `alp_handle_begin_close_blocking`. It never asked the adjacent question: **what about a callback the backend registered, which is not an in-flight op and which the drain therefore does not wait for?** Task 1 writes the invariant down and adds the checklist item — it is a design task with a documentation deliverable, deliberately separated because three of the four fixes are one-liners whose *justification* is the hard part. Tasks 2-4 fix the sites.

**Tech Stack:** C (clang-format 22.x, tabs), Zephyr, ztest, twister on `native_sim/native/64`.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections first, particularly the `#629 / #756 / #1114 / #1115` prior-art table and the note that the two `begin_close` variants are **not** interchangeable.

> **DESIGN PASS DONE 2026-08-24 — read before implementing. The invariant this
> plan proposes is only HALF the rule, and the highest-risk item is that Task 2
> as written leaves the exact bug it claims to close, behind a passing test.**
>
> **The invariant, in full, two arms.** *A slot may not return to its pool while
> anything outside the handle can still reach it: before `alp_slot_release()`,
> close must drain counted ops, sever every inbound reference (stop or deregister
> each callback source, clear each sibling back-pointer), and only then release.*
> *Where a callback source CANNOT be synchronously stopped — no cancel or
> deregister API, or an ISR the closing context cannot mask — the slot's release
> must instead be **deferred until the source's last possible delivery has
> landed**, by parking the callback's landing site in slot-owned memory and
> accounting the armed callback so the close drain waits for it.*
>
> This plan writes only the first arm. `src/i2c_regfile.c:218-228` generalises to
> the class where a synchronous stop exists, and does NOT generalise to Zephyr's
> GATT client — `bt_gatt_read`/`bt_gatt_write` have no cancel — which is exactly
> the #1620 family. Shipping the one-arm text into `src/common/alp_slot_claim.h`
> teaches the next dispatcher author a rule that is *unachievable* at the sites
> where the bugs are worst, and they will improvise.
>
> - **Class A (stoppable source)** — i2c_regfile, the mbox registration, the
>   counter alarm (#1627), the UART RX ringbuf, LVGL user-data. Order:
>   drain, then stop, then clear back-pointers, then release.
> - **Class B (unstoppable source)** — the GATT client ops (#1620), any host API
>   with fire-and-forget completion. Ctx lives in the pool slot; an in-flight
>   marker is set when the callback is armed and cleared only by its final
>   delivery; release is gated on that marker.
>
> **No new helper.** The executable form of Class B needs none: *count the armed
> callback in `active_ops`* — enter when armed, leave at final delivery — and the
> existing `alp_handle_begin_close_blocking()` drain already waits for it. A
> `alp_handle_quiesce(stop_fn, ctx)` wrapper would be an interface with a
> different one-off implementation per site (an LVGL user-data clear, a
> `mbox_register_callback(NULL)`, a pool walk) and could enforce nothing the call
> site does not already have to write. The back-pointer half is unenforceable by
> any counter — no counter can see `c->state.radio` — so that stays prose plus
> the skill checklist row. Task 1's venue choice is right; only its text needs
> the second arm.
>
> **A second reference implementation this plan missed.**
> `alp_uart_rx_ringbuf_detach` (`src/backends/uart/zephyr_drv.c:306-323`) already
> does stop-IRQ, deregister, clear-back-ref, release — and its `:318` guard
> ("only clear if it still points at THIS handle") is a recycled-slot subtlety
> the invariant text should absorb. Cite it alongside `i2c_regfile.c`.
>
> **Site-by-site, with three corrections:**
>
> 1. **`src/ble_dispatch.c` — both halves verified, but the plan walks the wrong
>    number of teardown sites.** `_free_conn` really is reached only from `:536`
>    and `:570`; `c->state.radio = h` at `:533`; radio close (`:284-326`) never
>    touches `_conn_pool`. **HIGHEST-RISK ITEM IN THIS PLAN:** the radio is torn
>    down in **two** places — `alp_ble_close`'s `CLOSE_NOW` path (`:321-325`) AND
>    the deferred self-close inside `alp_ble_scan_start` (`:479-489`, the #756
>    machinery). The plan adds the walk only to the former, so a close triggered
>    from inside a scan callback — the exact reentrant path #756 was built for —
>    still frees the radio at `:488` without walking the pool. Every conn dangles
>    and the slots strand, i.e. the bug the task claims to close, now hidden
>    behind a green test, because the ztest closes externally and passes. Factor
>    the teardown into one static and call it from both.
>
>    **The plan's own deadlock hedge is wrong — do not follow it.** Walking
>    *before* the radio's `begin_close` reopens the race it exists to close: an
>    `alp_ble_connect` counted on the radio can allocate a fresh conn after the
>    walk and before the CAS. The feared deadlock does not exist —
>    `alp_ble_disconnect` drains only `conn->active_ops` (`:564`), which the
>    closing thread holds no count on; worst case it blocks up to a GATT
>    `timeout_ms`, which is the documented sleep-poll contract. Walk **after** the
>    CAS (no new conns can be created for a CLOSING radio: `alp_ble_connect`'s
>    `op_enter` at `:519` fails), **before** `h->state.ops->close`.
>
>    One honest nuance for the PR: the permanent-`ALP_ERR_NOMEM` half is true for
>    an app that treats its conn handles as dead after radio close (the natural
>    contract), but strictly `alp_ble_disconnect` on a stale conn still works,
>    because `c->state.ops` was copied at `:532` and disconnect never dereferences
>    the radio.
>
> 2. **`src/gui_lvgl.c` — right fix, wrong facts, unimplementable sketch.** The
>    two `alp_gui_lvgl_attach` definitions at `:107`/`:157` are **not** an
>    LVGL-version pair: the guard is `ALP_HAS_LVGL` real-bridge vs link-stub
>    (`:155`'s `#else` returns `ALP_ERR_NOSUPPORT`). "Both arms" therefore means
>    only that the stub arm needs a stub `alp_gui_lvgl_detach` so the symbol
>    always links. The real gap: **the detach as sketched cannot be written** —
>    attach stores neither the `lv_display_t *` nor the malloc'd buffer (`:123`,
>    `:139`) anywhere reachable, so `alp_gui_lvgl_detach(void)` has nothing to
>    look up. The fix must add a static (and then decide whether a second attach
>    is rejected — today it silently creates a second display) or change the
>    signature; a detach that only clears user-data leaks the display object and
>    the buffer on every attach/detach cycle. Note also that
>    `alp_display_close` cannot stop this source at all (dispatch does not know
>    LVGL exists), so this site is Class A enforced as an **app-facing ordering
>    contract** — detach-before-close, documented on both symbols — plus the
>    non-optional NULL guard in `_flush_cb`. The SDK cannot make it
>    self-enforcing.
>
> 3. **`src/backends/mproc/zephyr_drv.c:443` — plan is right as written.** The
>    one-line `mbox_register_callback(be->dev, be->channel, NULL, NULL)` between
>    disable and `_mbox_be_free` is Class A textbook, and the "defence in depth,
>    do not overclaim" framing is correct for single-core Zephyr images.
>
> 4. **`src/zephyr/handles.c:29` — NOT REACHABLE, and the premise is false. Close
>    it with a comment.** The macro's `(struct type){0}` cannot "wipe the slot's
>    `lifecycle` and `active_ops`" because **neither pooled struct has those
>    fields**: `struct alp_adc_stream` (`src/zephyr/handles.h:149-156`) and
>    `struct alp_uart_rx_ringbuf` (`:91-96`) carry `in_use` plus plain data only.
>    The real defect in those classes is #1634's — no lifecycle guard at all
>    (e.g. `alp_uart_rx_ringbuf_pop` at `src/backends/uart/zephyr_drv.c:292` reads
>    `rb->in_use` unguarded against a racing detach's `lwrb_free`). Once #1634
>    adds `lifecycle`/`active_ops` and a drained close, `in_use == false` will
>    imply zero in-flight ops and the re-own zeroing becomes provably safe.
>    Deliverable here: a comment recording that, pointing at #1634. Do not
>    convert the mutex claim to a CAS — #1630 allowlists it correctly.
>
> **Relationship to #1620.** Its shape (pool-resident ctx, per-conn in-flight flag
> held across a timeout, cleared by the late callback, guard checked before
> anything reaches the host) is neither a special case of drain-stop-release nor a
> different invariant — it IS the Class B arm, and Task 1's header text should
> cite it as the Class B reference exactly as `i2c_regfile.c` is cited for Class
> A. Do not refactor either onto the other now. One unification worth a follow-up
> **on #1620, not here**: if its in-flight flag were folded into
> `conn->active_ops`, `alp_ble_disconnect`'s existing drain (`:564`) would wait
> for the late callback with zero new machinery — valid only if Zephyr guarantees
> pending GATT params complete (with error) on disconnect, which must be verified
> against the host source first.
>
> **Caveat on this design pass:** the advisor's checkout predated #1620 landing,
> so its Class-B reading of that fix came from a description rather than the
> diff. Confirm against the merged PR before citing it in the header text.

## Global Constraints

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 280 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` green before `gh pr create`.
- clang-format **22.x** on every changed `.c`/`.h` including test files.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result.
- No AI attribution anywhere.
- **Depends on Plan 1 landing first.** #1620 (the GATT stack-ctx critical) edits `src/backends/ble/zephyr_drv.c`, and Task 2 here edits `src/ble_dispatch.c`. Different files, but the same subsystem and the same reviewer — sequencing them avoids two people reasoning about BLE lifetime from opposite ends.

---

## The invariant, and where it already exists

Exactly one file in the tree states the rule, and it states it correctly:

```c
/* src/i2c_regfile.c:218-228 */
	/* Drain in-flight ops, then stop the wrapped target (guarantees no
	 * callback fires afterward) before releasing the slot -- so neither a
	 * synchronous op nor an ISR callback touches a recycled slot. #629 */
	if (!alp_handle_begin_close_blocking(&rf->lifecycle, &rf->active_ops)) {
		return;
	}
	alp_i2c_target_close(rf->tgt); /* no callback fires after this */
	rf->tgt  = NULL;
	rf->regs = NULL;
	alp_lifecycle_set(&rf->lifecycle, ALP_HANDLE_LC_UNOPENED);
	alp_slot_release(&rf->in_use);
```

Note the ordering, which is the whole point: **drain, then stop the callback source, then release the slot.** Draining alone is not enough — the drain waits for `active_ops` to reach zero, and a registered callback is not an op.

### The four sites that do not follow it

| # | Site | What still points at the freed slot |
|---|---|---|
| 1 | `src/ble_dispatch.c` radio close | Every live `struct alp_ble_conn`'s `state.radio` back-pointer, **and the conn slots themselves leak** |
| 2 | `src/gui_lvgl.c:128` | LVGL's `lv_display_t` user-data holds a raw `alp_display_t *` forever |
| 3 | `src/backends/mproc/zephyr_drv.c:443` | A `mbox_register_callback` registration whose `user_data` is the freed `be` |
| 4 | `src/zephyr/handles.c:29` | A slot re-owned by a new opener while an op may still be in flight |

Three more instances of this class are already filed separately and are **not** in scope here — reference them, do not restate them:

- **#1627** — counter `z_close` stops the counter but never cancels the armed channel alarm.
- **#1620** — `z_gatt_read`/`z_gatt_write` leave an on-stack ctx registered with the BT host (release-blocker, Plan 1 Task 3).
- **#1634** — ADC stream/filter/spectrum handles carry no `lifecycle`/`active_ops` at all.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `docs/adr/` or `src/common/alp_slot_claim.h` | Modify: write the cross-handle invariant down where a dispatcher author will meet it | 1 |
| `src/ble_dispatch.c` | Modify: radio close walks `_conn_pool`, closes each live conn, clears `state.radio` | 2 |
| `tests/zephyr/peripheral/src/ble_lifetime.c` | Create: ztest for the conn-slot leak | 2 |
| `src/gui_lvgl.c` | Modify: add `alp_gui_lvgl_detach()`; clear the LVGL user-data back-ref | 3 |
| `include/alp/gui.h` | Modify: declare the detach entry point | 3 |
| `src/backends/mproc/zephyr_drv.c:443` | Modify: unregister the mbox callback before freeing its user data | 4 |
| `src/zephyr/handles.c:29` | Modify: do not re-own a slot whose op may be in flight | 4 |

---

## Task 1: Write the invariant down

**Files:** `src/common/alp_slot_claim.h` (the header every dispatcher already includes), plus a checklist row in the `writing-race-safe-dispatch-handlers` skill.

**Interfaces:** produces no code. Produces the sentence Tasks 2-4 cite.

**Why this is a task and not a preamble.** Three of the four fixes below are one to five lines. Anyone can write them. What makes them *stick* is that the next dispatcher author knows to ask the question — and right now the only place the answer exists is a comment inside `src/i2c_regfile.c`, which nobody reads unless they are working on the I2C register-file wrapper.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/1644-cross-handle-lifetime origin/dev
```

- [ ] **Step 2: Add the invariant to `src/common/alp_slot_claim.h`**

Extend the file's existing header comment — the one that already explains the #629 / #756 / #1114 close protocol — with the adjacent question it does not currently ask:

```c
 * CROSS-HANDLE LIFETIME (issue #1644).  Draining active_ops answers
 * "is an operation in flight?".  It does NOT answer "does anything
 * still hold a pointer into this slot?".  A registered callback -- a
 * driver alarm, an ISR trampoline, a mailbox handler, a third-party
 * library's user-data -- is not an in-flight op, so the drain does not
 * wait for it and close() will happily free a slot it points at.
 *
 * Before alp_slot_release(), a close path MUST also:
 *   1. stop the callback source (cancel the alarm, unregister the
 *      handler, disable the IRQ), and
 *   2. clear any back-pointer another live handle holds into this one.
 *
 * src/i2c_regfile.c:218-228 is the reference implementation:
 * drain -> stop the callback source -> release the slot, in that order.
 * Releasing before stopping is the bug; so is stopping without
 * clearing a sibling handle's back-pointer.
```

- [ ] **Step 3: Add the checklist row to the skill**

The `alp-lab:writing-race-safe-dispatch-handlers` skill is where a dispatcher author is sent. Add a row asking, for every `close()`:

> Does anything outside this handle still point at its slot — a driver callback registered with the slot as `user_data`, an ISR trampoline, a sibling handle's back-pointer? If so, stop it and clear it **before** `alp_slot_release()`, per `src/common/alp_slot_claim.h`'s cross-handle-lifetime note.

The skill lives outside the repo checkout (`alp-lab` plugin), so this edit is not part of the PR — do it separately and note in the PR body that it was done. **Do not skip it**: the code comment alone repeats the failure mode of the `i2c_regfile.c` comment nobody found.

- [ ] **Step 4: Commit**

```bash
clang-format -i src/common/alp_slot_claim.h
git diff --exit-code
git add src/common/alp_slot_claim.h
git commit -m "docs(common): state the cross-handle lifetime invariant

#629 made slots safe against an in-flight operation racing close(). It never
asked the adjacent question: a registered callback is not an in-flight op, so
draining active_ops does not wait for it and close() frees a slot it still
points at. The rule existed only as a comment in src/i2c_regfile.c."
```

---

## Task 2: BLE radio close orphans every live connection

**Files:** `src/ble_dispatch.c`, `tests/zephyr/peripheral/src/ble_lifetime.c` (create).

**Interfaces:** consumes `alp_handle_begin_close_selfaware` / `alp_slot_release` from `src/common/alp_slot_claim.h`.

**The defect.** `alp_ble_close()` frees the radio slot and never walks the connection pool:

```c
/* src/ble_dispatch.c:318-325 */
	if (mode == ALP_HANDLE_CLOSE_DEFERRED) {
		return;
	}
	if (h->state.ops != NULL && h->state.ops->close != NULL) {
		h->state.ops->close(&h->state);
	}
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free_radio(h);
}
```

The connection pool is a separate static array:

```c
/* src/ble_dispatch.c:104 */
static struct alp_ble_conn _conn_pool[CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES];
```

Each connection stores a raw back-pointer to the radio:

```c
/* src/ble_dispatch.c:533 */
		c->state.radio = h;
```

and the only thing that returns a conn slot is:

```c
/* src/ble_dispatch.c:138-141 */
static void _free_conn(struct alp_ble_conn *h)
{
	alp_slot_release(&h->in_use);
}
```

**`_free_conn` is called from exactly two places** — `src/ble_dispatch.c:536` (the connect-failure path) and `:570` (`alp_ble_disconnect`). Verified:

```bash
grep -n "_free_conn" src/ble_dispatch.c
# 138: definition
# 536: connect-failure rollback
# 570: alp_ble_disconnect
```

So closing the radio with connections open produces two defects at once:

1. Every live `c->state.radio` dangles — a subsequent call on that conn dereferences a freed radio slot, which may already have been re-claimed by a new `alp_ble_open()`.
2. The conn slots leak. After `CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES` open/connect/close cycles the pool is exhausted and `alp_ble_connect()` returns `ALP_ERR_NOMEM` **forever**, with no way to recover short of a reboot.

The leak is the half a customer hits first, and it looks like a resource leak rather than a lifetime bug, which is why it has survived.

- [ ] **Step 1: Write the failing test**

Create `tests/zephyr/peripheral/src/ble_lifetime.c`. The leak is deterministically reproducible on `native_sim` without a peer — that is what makes it the right assertion:

```c
#include <zephyr/ztest.h>

#include <alp/ble.h>
#include <alp/peripheral.h>

/* Closing the radio with no explicit disconnect must not strand the conn
 * slots: after more open/close cycles than the pool has entries, a fresh
 * connect must still be able to claim one. */
ZTEST(alp_peripheral, test_ble_radio_close_reclaims_conn_slots)
{
	for (int cycle = 0; cycle < (CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES + 2); ++cycle) {
		alp_ble_t *radio = alp_ble_open(NULL);
		if (radio == NULL) {
			ztest_test_skip(); /* no BLE backend in this image */
		}

		/* Claim a conn slot without needing a real peer. If the backend
		 * cannot fabricate one on native_sim, fall back to asserting the
		 * pool is intact after close (see the note below). */
		alp_ble_close(radio);
	}

	/* The pool must not be exhausted by the loop above. */
	alp_ble_t *radio = alp_ble_open(NULL);
	zassert_not_null(radio, "radio pool exhausted after %d open/close cycles",
	                 CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES + 2);
	alp_ble_close(radio);
}

ZTEST(alp_peripheral, test_ble_conn_backptr_cleared_on_radio_close)
{
	/* A conn that outlives its radio must not hold a dangling state.radio.
	 * Reachable only if a conn can be established on this platform. */
	ztest_test_skip(); /* requires a peer; see the bench step */
}
```

**Read `alp_ble_connect`'s signature and the backend's native_sim behaviour before finalising this test.** If a conn slot cannot be claimed without a peer, the first test still holds (it exercises the radio pool and the close path) but does not prove the conn-slot reclaim. In that case say so plainly in the PR and lean on the bench step — do **not** write an assertion that passes for the wrong reason.

- [ ] **Step 2: Run it and confirm it FAILS**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

Record the actual failure mode. If it passes because no conn was ever claimed, that is information — the test is not yet exercising the defect, and Step 1's fallback applies.

- [ ] **Step 3: Walk the conn pool on radio close**

In `alp_ble_close()`, before `_free_radio(h)`, close every connection that belongs to this radio. Follow the invariant's ordering — stop the callback source and clear the back-pointer, then release:

```c
	/* Cross-handle lifetime (#1644): a conn holds a raw back-pointer to
	 * this radio (c->state.radio, set at connect). Freeing the radio slot
	 * without walking the pool leaves every live conn dangling AND strands
	 * its slot -- _free_conn is otherwise only reached from the
	 * connect-failure path and alp_ble_disconnect. */
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES; ++i) {
		struct alp_ble_conn *c = &_conn_pool[i];
		if (!__atomic_load_n(&c->in_use, __ATOMIC_ACQUIRE)) {
			continue;
		}
		if (c->state.radio != h) {
			continue; /* belongs to a different radio handle */
		}
		alp_ble_disconnect(c); /* stops the backend's callbacks, frees the slot */
	}
```

**Two things to get right, and both are easy to get wrong:**

- Use `alp_ble_disconnect(c)` rather than a bare `_free_conn(c)`. The bare release skips the backend teardown, so the BT host keeps its own registration and you have converted a leak into a use-after-free. Read `alp_ble_disconnect` at `src/ble_dispatch.c:~560-572` and confirm it does the full teardown before relying on this.
- `alp_ble_disconnect` may itself take the conn's lifecycle CAS and could interact with the radio's own close guard. Check whether calling it from inside the radio close path can deadlock — if the radio close used `alp_handle_begin_close_selfaware` and the conn path drains blocking, that is exactly the reentrancy #756 exists for. If it can, do the walk **before** the radio's `begin_close`, not after.

- [ ] **Step 4: Run the test and confirm it PASSES**

- [ ] **Step 5: Format, gate, commit**

```bash
clang-format -i src/ble_dispatch.c tests/zephyr/peripheral/src/ble_lifetime.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git add src/ble_dispatch.c tests/zephyr/peripheral/src/ble_lifetime.c \
        tests/zephyr/peripheral/CMakeLists.txt
git commit -m "fix(ble): radio close must walk the conn pool

alp_ble_close() freed the radio slot without touching _conn_pool. _free_conn is
reached only from the connect-failure path and alp_ble_disconnect, so closing a
radio with connections open left every c->state.radio dangling at a slot that a
later alp_ble_open() could re-claim, and stranded the conn slots -- after
CONFIG_ALP_SDK_MAX_BLE_CONN_HANDLES cycles alp_ble_connect() returned
ALP_ERR_NOMEM permanently."
```

- [ ] **Step 6: Bench verification — E1M-AEN801**

The dangling-back-pointer half needs a real peer. Connect, then call `alp_ble_close(radio)` **without** disconnecting first, and confirm the connection tears down cleanly rather than faulting. Batch this into the same E1M-AEN801 session as Plan 1's #1618 and #1620 — all three are BLE/GPIO work on the same board.

---

## Task 3: LVGL holds a raw display handle for the process lifetime

**Files:** `src/gui_lvgl.c`, `include/alp/gui.h`.

**The defect.** The bridge parks the portable handle in LVGL's display object:

```c
/* src/gui_lvgl.c:128 */
	lv_display_set_user_data(disp, display);
```

and the flush callback reads it back on every blit:

```c
/* src/gui_lvgl.c:94-96 */
static void _flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
	alp_display_t *display = (alp_display_t *)lv_display_get_user_data(disp);
```

`alp_gui_lvgl_attach()` has no matching detach. The file acknowledges this at `:145-146`:

> `/* Persistent for the process lifetime -- alp_gui_lvgl_attach() has no matching detach in the [ABI-STABLE] contract, so there is no ... */`

That reasoning is sound **for the frame buffer** it was written about. It was then applied to the handle pointer, which is not process-lifetime: if the display handle is closed and its pool slot re-used, `_flush_cb` blits into whatever now owns that slot.

**This repo has no active customers and no ABI-compat obligation** — the campaign's global constraints say so explicitly. The right answer is to add the detach, not to document around its absence.

- [ ] **Step 1: Note the two attach definitions**

`grep -n "alp_gui_lvgl_attach" src/gui_lvgl.c` reports **two** definitions, at `:107` and `:157` — almost certainly an `#if`/`#else` pair for LVGL major versions. Read the guard and make sure the detach is added to **both** arms. Fixing one arm and leaving the other is this campaign's signature failure mode.

- [ ] **Step 2: Declare the detach**

In `include/alp/gui.h`, beside `alp_gui_lvgl_attach`:

```c
/**
 * @brief Detach the LVGL bridge from its display and clear the back-reference.
 *
 * Must be called before @ref alp_display_close on the handle passed to
 * @ref alp_gui_lvgl_attach.  LVGL otherwise retains a raw `alp_display_t *`
 * in its display user-data and the flush callback blits into whatever
 * re-claims that pool slot (issue #1644).
 *
 * Idempotent: a second call, or a call with no attachment live, is a no-op.
 *
 * @return @ref ALP_OK, or @ref ALP_ERR_NOT_READY when nothing is attached.
 */
alp_status_t alp_gui_lvgl_detach(void);
```

**A new public symbol is an ABI change** — regenerate the ABI snapshot or `check · generated files in sync` goes red. `include/alp/gui.h` is public, so this one genuinely does need it, unlike Plans 2 and 6.

- [ ] **Step 3: Implement it in both arms**

Clear LVGL's user-data back-reference so a late flush cannot resolve a handle:

```c
alp_status_t alp_gui_lvgl_detach(void)
{
	lv_display_t *disp = /* the same display alp_gui_lvgl_attach() created//used */;
	if (disp == NULL) {
		return ALP_ERR_NOT_READY;
	}
	lv_display_set_user_data(disp, NULL);
	return ALP_OK;
}
```

and harden `_flush_cb` against the window where LVGL still calls back with a cleared pointer:

```c
	alp_display_t *display = (alp_display_t *)lv_display_get_user_data(disp);
	if (display == NULL) {
		return; /* detached (#1644) -- nothing to blit into */
	}
```

The NULL check is not optional. Clearing the pointer without it converts a use-after-free into a NULL deref, which is better but still a crash.

- [ ] **Step 4: Test, format, gate, commit**

Add a ztest asserting `alp_gui_lvgl_detach()` is idempotent and that a second call returns `ALP_ERR_NOT_READY`. The blit-after-free path itself is not reachable from a unit test — say so rather than faking it.

```bash
clang-format -i src/gui_lvgl.c include/alp/gui.h
python3 scripts/abi_snapshot.py --version "$(cat metadata/sdk_version.yaml | head -1)" --output docs/abi/…
git diff --exit-code
bash scripts/test-all.sh --target dev
```

Read `regenerating-generated-files` for the exact `abi_snapshot.py` invocation rather than guessing the arguments above.

```bash
git commit -am "feat(gui): add alp_gui_lvgl_detach()

alp_gui_lvgl_attach() parked a raw alp_display_t * in LVGL's display user-data
with no way to take it back, so closing the display left _flush_cb blitting
into whatever re-claimed that pool slot. The file documented this as acceptable
because the [ABI-STABLE] contract had no detach -- but that reasoning was
written about the frame buffer, which really is process-lifetime, and this repo
has no ABI-compat obligation."
```

---

## Task 4: The two remaining sites

**Files:** `src/backends/mproc/zephyr_drv.c`, `src/zephyr/handles.c`.

### 4a — mbox callback outlives its user data

```c
/* src/backends/mproc/zephyr_drv.c:443-454 */
static void z_mbox_close(alp_mbox_backend_state_t *state)
{
#if defined(CONFIG_ALP_SDK_MPROC)
	struct mbox_be *be = (struct mbox_be *)state->be_data;
	if (be == NULL) return;
	(void)mbox_set_enabled(be->dev, be->channel, false);
	_mbox_be_free(be);
	state->be_data = NULL;
```

The callback was registered with `be` as its user data:

```c
/* src/backends/mproc/zephyr_drv.c:431 */
	int err     = mbox_register_callback(be->dev, be->channel, cb ? mbox_rx_cb : NULL, be);
```

`mbox_set_enabled(..., false)` at `:449` is the documented Zephyr way to stop delivery, so this is **less** exposed than the counter alarm — it may well be safe today. But the registration itself survives with a freed `user_data`, so anything that re-enables the channel (a peer-driven path, a later open of the same channel) resurrects a pointer to a released pool entry.

- [ ] **Step 1: Unregister before freeing**

```c
	(void)mbox_set_enabled(be->dev, be->channel, false);
	(void)mbox_register_callback(be->dev, be->channel, NULL, NULL);
	_mbox_be_free(be);
```

One line, and it makes the close path self-evidently correct rather than correct-by-argument-about-Zephyr-internals. Note in the commit that this is defence in depth, not a demonstrated live crash — do not overclaim it.

### 4b — `handles.c` re-owns a slot under an in-flight op

```c
/* src/zephyr/handles.c:29 (inside the pool-alloc macro) */
			if (!kind##_slots[i].in_use) { \
				kind##_slots[i]        = (struct type){ 0 }; \
				kind##_slots[i].in_use = true; \
```

The whole loop runs inside `k_mutex_lock(&kind##_lock, K_FOREVER)` (`src/zephyr/handles.c:25`), so **the claim itself is correct** and this is explicitly *not* a slot-claim defect — Plan 2 (#1630) allowlists it for exactly that reason. Do not convert it to a CAS.

The separate problem is the struct-assignment zeroing: a new opener wipes the slot's `lifecycle` and `active_ops` while a previous holder's operation may still be running against it. That is this issue's class, not #1630's.

- [ ] **Step 2: Establish whether it is reachable**

Before changing anything, determine whether a slot can reach `in_use == false` while an op is still in flight. If every close path here goes through `alp_handle_begin_close_blocking` (which drains `active_ops` to zero **before** releasing), it cannot, and the correct outcome of this step is a comment recording that — not a change.

```bash
grep -n "in_use = false\|alp_handle_begin_close\|active_ops" src/zephyr/handles.c
```

**If it is not reachable, say so and close the item.** A defensive change to a macro used by every Zephyr handle class, made without a demonstrated path, is churn with a blast radius.

- [ ] **Step 3: Format, gate, commit**

```bash
clang-format -i src/backends/mproc/zephyr_drv.c src/zephyr/handles.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "fix(mproc): unregister the mbox callback before freeing its user data

z_mbox_close disabled the channel but left the registration in place with a
freed struct mbox_be as its user_data, so any path that re-enables the channel
resurrects a pointer to a released pool entry. Defence in depth -- disabling
first makes a live crash unlikely today."
```

---

## Opening the PRs

Four PRs, all `--base dev`, in task order.

- Task 1: `Refs #1644.` Labels `documentation`, `area:portability`.
- Task 2: `Refs #1644.` Labels `bug`, `area:drivers`, `aen`, `needs-silicon`.
- Task 3: `Refs #1644.` Labels `bug`, `area:drivers`. **ABI snapshot regen required.**
- Task 4: `Closes #1644.` Labels `bug`, `area:drivers`.

**Bench:** Task 2's dangling-back-pointer half needs a peer on E1M-AEN801. Batch it with Plan 1's #1618 and #1620 — all three are the same board and the same subsystem area, and reserving once for three fixes is the difference between one bench session and three.

**What this plan deliberately does not do:** #1627 (counter alarm), #1620 (GATT stack ctx), and #1634 (ADC stream lifecycle) are the same class and are filed separately. Fixing them here would make one PR that three different reviewers each half-understand.
