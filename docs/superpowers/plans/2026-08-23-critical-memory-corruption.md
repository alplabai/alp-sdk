# Critical Memory Corruption — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the three confirmed memory-corruption defects in `src/backends/` — an out-of-bounds function pointer called from ISR context, an unbounded `memcpy` into a fixed slab block, and a stack context left registered with the Bluetooth host across a timeout.

**Architecture:** Three independent defects in three unrelated backends. Each is its own branch and its own PR; they share no file and have no ordering dependency between them. Task 1 is the smallest and should land first because it proves the branch/gate loop. **All three need an E1M-AEN801 bench pass** — see the correction below; batch them into one reservation.

> **CORRECTION (2026-08-23, found while implementing Task 1).** This plan originally said Task 1 needed no hardware. That was wrong. `test_i2s_write_rejects_oversize_block` **skips** on `native_sim` (`ztest skip`, all four `tests/zephyr/peripheral` scenarios) because `alp_i2s_open()` returns NULL there, so it never exercises the guard.
>
> The reason is a *different* confirmed defect: `src/i2s_dispatch.c:65` calls `alp_backend_select("i2s", ALP_SOC_REF_STR)` with **no `alp_backend_select_next` fall-through**. `zephyr_drv` (priority 100, compiled under `CONFIG_I2S`) wins selection, its `z_open` fails `ALP_ERR_NOT_READY` because `_devs[0]` is NULL on `native_sim`, and the dispatcher never falls back to `sw_fallback` (priority 0, `silicon_ref = "*"`). That is the "fall-through used in 2 of 33 dispatchers" pattern from **#1635 / #1641**, observed live.
>
> There is no workaround at this layer: Zephyr ships **no `i2s_emul`**; the `vnd_i2s` build-all stub's `vnd_i2s_configure()` returns `-ENOTSUP`, so `z_open` fails against it too; and `src/backends/i2s/sw_fallback.c`'s `write` returns `ALP_ERR_NOSUPPORT` **by deliberate design** — its header states that faking I2S frame movement "would tempt callers to validate a SW loopback that the production backend does not provide."
>
> Keep the test. It is correct, it skips cleanly, and it starts exercising the guard the moment a real I2S device is present. But **do not report a green `native_sim` run as verification of this fix** — the run passes whether or not the guard exists. Verify on silicon.
>
> Two incidental findings from the same investigation, worth filing against #1641 and #1635 respectively: the i2s dispatcher fall-through gap above, and `zephyr/CMakeLists.txt:1472`, whose comment says `sw_fallback` is "gated on `CONFIG_ALP_SDK_I2S_SW_FALLBACK`" while line 1480 compiles it unconditionally.

**Tech Stack:** C (clang-format 22.x, tabs), Zephyr RTOS, ztest, twister on `native_sim/native/64`, J-Link SWD for the bench half.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections before Task 1; every task below assumes them.

## Global Constraints

Inherited verbatim from the campaign index. Repeated here because an executor may read this file alone:

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev` before every `gh pr create`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 282 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` must be green before `gh pr create`. Not a subset.
- clang-format is **22.x** — tabs, Consecutive alignment, BinPack off. Format every changed `.c`/`.h` **including test files**.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result even if this change did not touch it.
- A new public symbol or macro is an ABI change — regenerate the ABI snapshot or `check · generated files in sync` goes red.
- No AI attribution in any commit message, PR body, or issue.
- `native_sim` passing is **not** evidence that an ISR fires on an M55. Tasks marked `needs-silicon` do not merge without a bench log.

---

## File Structure

| File | Responsibility | Task |
|---|---|---|
| `src/backends/i2s/zephyr_drv.c` | Modify: store the negotiated slab block size, reject an oversize write | 1 |
| `tests/zephyr/peripheral/src/i2s.c` | Modify (or create): ztest for the oversize-write rejection | 1 |
| `tests/zephyr/peripheral/CMakeLists.txt` | Modify: register the test source if newly created | 1 |
| `src/backends/gpio/gpio_ops.h` | Modify: add the delegated-open entry point to `alp_gpio_ops_t` | 2 |
| `src/backends/gpio/zephyr_drv.c` | Modify: implement delegated open; stop deriving `owner` by `CONTAINER_OF` on the delegated path | 2 |
| `src/backends/gpio/cc3501e_proxy.c` | Modify: pass the real owner handle when delegating | 2 |
| `tests/zephyr/peripheral/src/gpio_proxy.c` | Create: ztest proving the delegated handle's callback fires on the right handle | 2 |
| `src/backends/ble/zephyr_drv.c` | Modify: move the GATT read/write ctx into the per-conn pool; add an in-flight guard | 3 |
| `tests/zephyr/peripheral/src/ble_gatt.c` | Create: ztest for the timeout path leaving no live registration | 3 |

---

## Task 1: Bound `z_write` against the negotiated slab block size

**Issue: #1619** (`release-blocker`, milestone `v0.17.0`)

**Files:**
- Modify: `src/backends/i2s/zephyr_drv.c:55-60` (struct), `:153-155` (open), `:206-224` (`z_write`)
- Test: `tests/zephyr/peripheral/src/i2s.c`

**Interfaces:**
- Consumes: nothing from other tasks.
- Produces: nothing other tasks rely on. Fully self-contained.

**The defect.** `z_write` copies a caller-supplied byte count into a `k_mem_slab` block whose size was fixed at `open()` and then discarded:

```c
/* src/backends/i2s/zephyr_drv.c:55-60 — block_bytes is NOT a member */
typedef struct {
	struct k_mem_slab mem_slab;
	uint8_t          *slab_buf;
	size_t            slab_buf_bytes;
	bool              in_use;
} alp_z_i2s_side_t;
```
```c
/* src/backends/i2s/zephyr_drv.c:153-155 — computed, used, thrown away */
	size_t block_bytes =
	    (size_t)cfg->block_frames * (size_t)cfg->channels * (size_t)((cfg->word_bits + 7u) / 8u);
	s->slab_buf_bytes = block_bytes * 2u;
```
```c
/* src/backends/i2s/zephyr_drv.c:218 — nothing to compare against */
	memcpy(slab_block, block, bytes);
	err = i2s_write(dev, slab_block, bytes);
```

Zephyr's own `i2s_write` would reject `bytes > block_size` with `-EINVAL`, but it runs on the line **after** the overflowing `memcpy`. The dispatcher validates only non-NULL and non-zero (`src/i2s_dispatch.c:132`), and the portable audio path reaches it with no I2S knowledge at all: `src/backends/audio/zephyr_drv.c:510` computes `bytes = frames * bytes_per_frame` and line 547 passes it straight through, while `src/audio_dispatch.c:299` checks only `buf != NULL` and `frames != 0`. Result: slab-neighbour and `k_malloc` heap corruption from a portable API call.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/i2s-write-block-bounds origin/dev
```

- [ ] **Step 2: Write the failing test**

Append to `tests/zephyr/peripheral/src/i2s.c` (create the file with the include block if it does not exist):

```c
#include <zephyr/ztest.h>
#include "alp/i2s.h"
#include "alp/peripheral.h"
#include "alp/soc_caps.h"

ZTEST(alp_peripheral, test_i2s_write_rejects_oversize_block)
{
	alp_i2s_config_t cfg = ALP_I2S_CONFIG_DEFAULT(0u);
	cfg.direction        = ALP_I2S_DIR_TX;
	cfg.block_frames     = 256u;
	cfg.channels         = 2u;
	cfg.word_bits        = 16u;
	cfg.sample_rate_hz   = 48000u;

	alp_i2s_t *h = alp_i2s_open(&cfg);
	if (h == NULL) {
		ztest_test_skip(); /* no i2s_emul on this platform */
	}

	/* Negotiated block is 256 * 2 * 2 = 1024 bytes. */
	static uint8_t buf[2048];
	zassert_equal(alp_i2s_write(h, buf, sizeof(buf), 100u), ALP_ERR_OUT_OF_RANGE,
	              "a write larger than the negotiated block must be rejected, not memcpy'd");

	/* A correctly sized write must still be accepted. */
	zassert_not_equal(alp_i2s_write(h, buf, 1024u, 100u), ALP_ERR_OUT_OF_RANGE,
	                  "an exactly-block-sized write must not be rejected");

	alp_i2s_close(h);
}
```

- [ ] **Step 3: Register the test source (only if the file was newly created)**

In `tests/zephyr/peripheral/CMakeLists.txt`, add to the existing `target_sources(app PRIVATE ...)` list:

```cmake
	src/i2s.c
```

- [ ] **Step 4: Run the test — expect SKIP, not FAIL**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

**Observed 2026-08-23: `skipped -- ztest skip`, in all four scenarios.** `alp_i2s_open()` returns NULL on `native_sim` for the dispatcher reason in the correction at the top of this plan, so the test cannot reach the guard. Confirm you see the same, then proceed — but record in the PR that the test skips and that the real evidence is Step 13's bench log. Do **not** treat a green run as verification.

Read the per-testcase status from the twister report, not the summary line: a skipped test still counts inside "N of N passed". Use `-O <dir>` and inspect `<dir>/twister.json`.

- [ ] **Step 5: Add the field to the side struct**

`src/backends/i2s/zephyr_drv.c:55-60` — add `block_bytes` **before** `in_use`. `in_use` must stay the last member: `_alloc_side()` memsets only up to `offsetof(..., in_use)` so the atomic claim is never transiently undone (the #1115 round-2 convention, same as `proxy_side_t` in `src/backends/gpio/cc3501e_proxy.c:73-78`).

```c
typedef struct {
	struct k_mem_slab mem_slab;
	uint8_t          *slab_buf;
	size_t            slab_buf_bytes;
	size_t            block_bytes; /**< Negotiated slab block size; the write bound. */
	bool              in_use;
} alp_z_i2s_side_t;
```

- [ ] **Step 6: Record the negotiated size at open**

`src/backends/i2s/zephyr_drv.c:155` — add the assignment next to the existing one:

```c
	s->slab_buf_bytes = block_bytes * 2u;
	s->block_bytes    = block_bytes;
```

- [ ] **Step 7: Bound the write BEFORE the memcpy**

`src/backends/i2s/zephyr_drv.c`, in `z_write`, insert the guard ahead of `k_mem_slab_alloc` so an oversize request never even claims a block:

```c
	alp_z_i2s_side_t    *s   = (alp_z_i2s_side_t *)st->be_data;
	const struct device *dev = (const struct device *)st->dev;
	if (s == NULL || dev == NULL) return ALP_ERR_NOT_READY;
	if (bytes > s->block_bytes) return ALP_ERR_OUT_OF_RANGE;
```

- [ ] **Step 8: Run the test and confirm it PASSES**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

Expected: `test_i2s_write_rejects_oversize_block` PASSES.

- [ ] **Step 9: Clamp the caller-side derivation too**

The audit's second half: `src/backends/audio/zephyr_drv.c:510` derives `bytes` from an unclamped `frames`. Read `z_out_write` and clamp `frames` against `state->cfg.frames_per_block` before the multiply, returning `ALP_ERR_OUT_OF_RANGE` when it exceeds. Without this the audio path just moves the rejection one layer out instead of never generating the oversize request.

- [ ] **Step 10: Check the sibling backend**

Grep the construct, not the instance:

```bash
grep -rn "memcpy(slab_block\|k_mem_slab_alloc" src/backends/
```

`src/backends/i2s/yocto_drv.c`'s `y_write` chunks its writes rather than assuming one block — confirm it does, and if it has the same unbounded shape, fix it in this PR and note it in the body.

- [ ] **Step 11: Format, then run the full gate set**

```bash
clang-format --version   # must report 22.x
clang-format -i src/backends/i2s/zephyr_drv.c src/backends/audio/zephyr_drv.c tests/zephyr/peripheral/src/i2s.c
git diff --exit-code     # must be empty after formatting
bash scripts/test-all.sh --target dev
```

- [ ] **Step 12: Commit**

```bash
git add src/backends/i2s/zephyr_drv.c src/backends/audio/zephyr_drv.c \
        tests/zephyr/peripheral/src/i2s.c tests/zephyr/peripheral/CMakeLists.txt
git commit -m "fix(i2s): bound alp_i2s_write against the negotiated slab block size

z_write memcpy'd a caller-supplied byte count into a k_mem_slab block whose
size was fixed at open() and then discarded, corrupting the neighbouring slab
block and the k_malloc heap. Zephyr's i2s_write would have rejected the
oversize length, but it runs after the memcpy.

Store block_bytes on alp_z_i2s_side_t and reject bytes > block_bytes with
ALP_ERR_OUT_OF_RANGE before allocating. Also clamp frames in the audio
backend's z_out_write so the portable path stops generating the request."
```

---

## Task 2: Stop passing a non-handle to the platform GPIO open

**Issue: #1618** (`release-blocker`, milestone `v0.17.0`)

**Files:**
- Modify: `src/backends/gpio/gpio_ops.h:45-46` (ops struct), `src/backends/gpio/zephyr_drv.c:163-180` (`z_open`), `src/backends/gpio/cc3501e_proxy.c:129-140` (`px_open`)
- Test: `tests/zephyr/peripheral/src/gpio_proxy.c`
- **`needs-silicon`: E1M-AEN801.**

**Interfaces:**
- Consumes: nothing from Task 1.
- Produces: a new member on `alp_gpio_ops_t`. Any other GPIO backend added later must either implement it or leave it `NULL` and be rejected on the delegated path — Task 2 Step 7 adds that guard.

**The defect.** The proxy hands the platform backend a pointer that is not a handle's `state` member:

```c
/* src/backends/gpio/cc3501e_proxy.c:73-78 */
typedef struct {
	bool                     is_bridge;
	uint8_t                  cc35_raw;
	alp_gpio_backend_state_t inner; /* delegated platform-backend state */
	bool                     in_use;
} proxy_side_t;
```
```c
/* src/backends/gpio/cc3501e_proxy.c:130-131 */
	const alp_gpio_ops_t *z  = alp_z_gpio_ops();
	alp_status_t          rc = z->open(pin_id, &s->inner, caps);
```

The platform backend then recovers an owner by container arithmetic:

```c
/* src/backends/gpio/zephyr_drv.c:174 */
	s->owner = CONTAINER_OF(st, struct alp_gpio, state);
```

`state` is the **first** member of `struct alp_gpio` (`src/backends/gpio/gpio_ops.h:54-55`), so `offsetof` is 0 and `owner` becomes `(struct alp_gpio *)&s->inner` — a `proxy_side_t` reinterpreted as a much larger `struct alp_gpio` whose `cb` field (`gpio_ops.h:61`) sits past the end of the slot, inside the **next** `_sides[]` entry.

That garbage is then called from interrupt context:

```c
/* src/backends/gpio/zephyr_drv.c:149-157 */
static void _isr_thunk(const struct device *port, struct gpio_callback *cb, gpio_port_pins_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	alp_z_gpio_side_t *s = CONTAINER_OF(cb, alp_z_gpio_side_t, zcb);
	struct alp_gpio   *h = s->owner;
	if (h != NULL && h->cb != NULL) {
		h->cb(h, h->cb_user);
	}
}
```

There is no guard anywhere: the dispatcher stores `cb` only in the real handle, and the platform backend explicitly declines to use its own arguments —

```c
/* src/backends/gpio/zephyr_drv.c:217-219 */
	(void)cb;
	(void)user; /* stashed in the portable handle by the dispatcher */
```

— which is exactly why the thunk *must* be given a real handle and cannot be fixed by having the proxy stash the callback locally.

**Reachable on shipping configurations.** `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y` appears in `examples/aen/aen-cc3501e-gpio/prj.conf:24`, `examples/aen/aen-cc3501e-bringup/prj.conf:22`, and `examples/aen/aen-cc3501e-companion-tour/prj.conf:22`, and **every non-routed pin takes the delegated path** — including the LEDs, `WIFI_EN`, and `nRESET`.

**Design decision (made, not deferred).** Two fixes were considered:

1. *Give the proxy a real `struct alp_gpio` to delegate through.* **Rejected** — the dispatcher populates `cb`/`cb_user` on the outer handle, so an inner handle's `cb` is always `NULL` and the interrupt would silently never fire. Safely broken is still broken.
2. *Add a delegated-open entry point that takes the owner explicitly.* **Chosen.** `px_open` already receives the real `alp_gpio_backend_state_t *state`, which genuinely is `&outer->state`, so the proxy can recover the true owner itself and pass it down. The proxy cannot simply forward `state` instead of `&s->inner`, because both layers write `state->be_data` and the platform backend would clobber the proxy's own sidecar pointer — hence a separate entry point rather than a signature change to `open`.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/gpio-proxy-delegated-owner origin/dev
```

- [ ] **Step 2: Write the failing test**

**Do NOT use the `alp/testing` GPIO double for this test.** The tree has a virtual GPIO backend with a clean edge injector — `alp_testing_gpio_edge(pin_id, edge)` in `include-testing/alp/testing/gpio.h:87`, exercised by `tests/zephyr/conformance/src/behavior_gpio.c:154` — and it is the wrong tool here. It registers at priority 255 and *replaces* the platform backend, which is precisely the backend the proxy delegates into. Selecting it would test the double's own callback plumbing and prove nothing about `CONTAINER_OF`. This test must run against the real `gpio_emul`-backed Zephyr backend with the proxy layered on top.

Create `tests/zephyr/peripheral/src/gpio_proxy.c`. It opens two delegated proxy handles so the adjacent `_sides[]` slot holds a recognisable pattern, arms an IRQ on the first, and asserts the callback receives the handle it was registered against — which the container-arithmetic bug cannot satisfy, because the thunk's `h` points at a `proxy_side_t`, not at a `struct alp_gpio`:

```c
#include <zephyr/ztest.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/gpio/gpio_emul.h>
#include <string.h>

#include <alp/peripheral.h>

static alp_gpio_t *g_seen_handle;
static void       *g_seen_user;
static int         g_fired;

static void proxy_cb(alp_gpio_t *pin, void *user)
{
	g_fired++;
	g_seen_handle = pin;
	g_seen_user   = user;
}

ZTEST(alp_peripheral, test_gpio_proxy_delegated_cb_gets_real_handle)
{
	/* Two handles: the second claims the _sides[] slot immediately after the
	 * first, so a cb read past the end of slot 0 lands in slot 1's bytes. */
	alp_gpio_t *a = alp_gpio_open(0u);
	alp_gpio_t *b = alp_gpio_open(1u);
	zassert_not_null(a, "delegated proxy open of pin 0 failed");
	zassert_not_null(b, "delegated proxy open of pin 1 failed");

	g_fired       = 0;
	g_seen_handle = NULL;
	g_seen_user   = NULL;

	zassert_equal(alp_gpio_irq_enable(a, ALP_GPIO_EDGE_RISING, proxy_cb, (void *)0x1234),
	              ALP_OK,
	              "irq_enable(RISING) on a delegated pin failed");

	/* Drive the edge on the underlying emulated port. Resolve the port/pin the
	 * same way the backend does -- see alp_z_gpio_resolve() in
	 * src/backends/gpio/zephyr_drv.c and mirror the DT spec lookup the
	 * existing tests/zephyr/peripheral/src/gpio.c uses for pin 0. */
	const struct device *port = DEVICE_DT_GET(DT_NODELABEL(gpio_emul0));
	zassert_true(device_is_ready(port), "gpio_emul0 not ready");
	zassert_ok(gpio_emul_input_set(port, 0, 0));
	zassert_ok(gpio_emul_input_set(port, 0, 1));
	k_msleep(10);

	zassert_equal(g_fired, 1, "cb did not fire exactly once on the armed rising edge");
	zassert_equal(g_seen_handle, a,
	              "cb fired with the wrong handle: the delegated path recovered an owner "
	              "by CONTAINER_OF on a proxy sidecar instead of the real handle");
	zassert_equal(g_seen_user, (void *)0x1234, "user cookie did not survive the delegation");

	zassert_equal(alp_gpio_irq_disable(a), ALP_OK, "irq_disable failed");
	alp_gpio_close(a);
	alp_gpio_close(b);
}
```

Two API facts this test depends on, both verified against the tree — do not "correct" them:
- `alp_gpio_t *alp_gpio_open(uint32_t pin_id);` (`include/alp/peripheral.h:265`) takes **one** argument. Direction and pull are set afterwards via `alp_gpio_configure`, not at open.
- `alp_gpio_irq_enable(alp_gpio_t *pin, alp_gpio_edge_t edge, alp_gpio_cb_t cb, void *user)` (`include/alp/peripheral.h:323`).

The existing suite compares handles with `zassert_equal`, not `zassert_equal_ptr` (`behavior_gpio.c:178`) — match that.

If the `DEVICE_DT_GET(DT_NODELABEL(gpio_emul0))` lookup does not match how `tests/zephyr/peripheral/src/gpio.c` reaches pin 0, use that file's approach instead; its comment at `:22-24` documents the `gpio_emul` read-back convention.

- [ ] **Step 3: Register the test source and its scenario**

In `tests/zephyr/peripheral/CMakeLists.txt`, add to the `target_sources(app PRIVATE ...)` list:

```cmake
	src/gpio_proxy.c
```

The proxy path needs its Kconfig on, and it must **not** be on for the other scenarios in this directory — enabling the proxy globally would route every existing GPIO test through it. Add a dedicated scenario to `tests/zephyr/peripheral/testcase.yaml`, following the `extra_args` pattern already used there:

```yaml
  alp_sdk.peripheral.cc3501e_proxy:
    platform_allow:
      - native_sim
      - native_sim/native/64
    extra_args:
      - "EXTRA_CONF_FILE=prj_cc3501e_proxy.conf"
    tags:
      - alp-sdk
      - peripheral
```

with `tests/zephyr/peripheral/prj_cc3501e_proxy.conf` containing:

```
CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y
```

Because the new test only compiles meaningfully under that scenario, guard the file body with `#ifdef CONFIG_ALP_SDK_GPIO_CC3501E_PROXY` so the other scenarios build it as an empty translation unit rather than failing.

- [ ] **Step 4: Run the test and confirm it FAILS**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

Expected: FAIL. Either `g_fired == 0` (the garbage `cb` read as `NULL`) or a crash inside `_isr_thunk` (the garbage `cb` read as a non-NULL address). **Both outcomes confirm the defect** — record which one you saw in the PR body, because it tells the reviewer what the adjacent slot happened to hold.

- [ ] **Step 5: Add the delegated-open entry point to the ops struct**

`src/backends/gpio/gpio_ops.h`, inside `struct alp_gpio_ops` (near the existing `open`):

```c
	/* Delegated open: used when the caller is another backend (the CC3501E
	 * proxy) whose sidecar holds the platform state, so `st` is NOT the
	 * `state` member of a struct alp_gpio and the owner cannot be recovered
	 * by CONTAINER_OF.  `owner` is the real portable handle whose cb/cb_user
	 * the ISR thunk must read.  NULL when a backend cannot be delegated to. */
	alp_status_t (*open_delegated)(uint32_t                  pin_id,
	                               alp_gpio_backend_state_t *st,
	                               struct alp_gpio          *owner,
	                               alp_capabilities_t       *caps_out);
```

- [ ] **Step 6: Implement it in the platform backend and route the plain open through it**

`src/backends/gpio/zephyr_drv.c` — factor the body of `z_open` so the owner is a parameter rather than a derivation, and keep `z_open` as the CONTAINER_OF-using wrapper for the normal (non-delegated) path:

```c
static alp_status_t z_open_delegated(uint32_t                  pin_id,
                                     alp_gpio_backend_state_t *st,
                                     struct alp_gpio          *owner,
                                     alp_capabilities_t       *caps_out)
{
	struct gpio_dt_spec spec;
	if (owner == NULL) return ALP_ERR_INVAL;
	if (!alp_z_gpio_resolve(pin_id, &spec)) return ALP_ERR_INVAL;
	if (!device_is_ready(spec.port)) return ALP_ERR_NOT_READY;

	alp_z_gpio_side_t *s = _alloc_side();
	if (s == NULL) return ALP_ERR_NOMEM;
	s->spec  = spec;
	s->owner = owner;

	st->dev         = (void *)spec.port;
	st->pin_id      = pin_id;
	st->be_data     = s;
	caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t
z_open(uint32_t pin_id, alp_gpio_backend_state_t *st, alp_capabilities_t *caps_out)
{
	return z_open_delegated(pin_id, st, CONTAINER_OF(st, struct alp_gpio, state), caps_out);
}
```

and add `.open_delegated = z_open_delegated,` to the ops table in the same file.

- [ ] **Step 7: Use it from the proxy, and reject a backend that cannot be delegated to**

`src/backends/gpio/cc3501e_proxy.c`, replacing lines 130-131. `state` here genuinely is `&outer->state`, so the container arithmetic is valid at *this* layer:

```c
	/* Not proxied (or no bridge attached): delegate to the platform driver.
	 * `state` IS the real handle's state member, so the owner recovered here
	 * is genuine -- unlike `&s->inner`, which is a sidecar member. */
	const alp_gpio_ops_t *z = alp_z_gpio_ops();
	if (z->open_delegated == NULL) return ALP_ERR_NOSUPPORT;
	struct alp_gpio *owner = CONTAINER_OF(state, struct alp_gpio, state);
	alp_status_t     rc    = z->open_delegated(pin_id, &s->inner, owner, caps);
	if (rc != ALP_OK) {
		_free_side(s);
		return rc;
	}
```

- [ ] **Step 8: Run the test and confirm it PASSES**

```bash
west twister -p native_sim/native/64 -T tests/zephyr/peripheral --no-clean -v
```

Expected: `test_gpio_proxy_delegated_cb_gets_real_handle` PASSES — `g_fired == 1`, `g_seen_handle == a`, `g_seen_user == &marker`.

- [ ] **Step 9: Audit the close path for the same arithmetic**

The audit reports `z_close` reading `s->owner->edge` out of bounds through the same bad pointer. Read `src/backends/gpio/zephyr_drv.c:250` and confirm it now reads a genuine owner. Then grep the construct across the backend directory — any other `CONTAINER_OF(st, struct alp_gpio, state)` on a path a delegating caller can reach is the same bug:

```bash
grep -rn "CONTAINER_OF(st\|CONTAINER_OF(state" src/backends/gpio/
```

- [ ] **Step 10: Format and run the full gate set**

```bash
clang-format -i src/backends/gpio/gpio_ops.h src/backends/gpio/zephyr_drv.c \
                src/backends/gpio/cc3501e_proxy.c tests/zephyr/peripheral/src/gpio_proxy.c
git diff --exit-code
bash scripts/test-all.sh --target dev
```

`gpio_ops.h` is internal, not under `include/alp/`, so this adds no public symbol and needs no ABI-snapshot regen. Confirm that by checking `git diff --stat` touches nothing under `include/`.

- [ ] **Step 11: Commit**

```bash
git add src/backends/gpio/gpio_ops.h src/backends/gpio/zephyr_drv.c \
        src/backends/gpio/cc3501e_proxy.c tests/zephyr/peripheral/src/gpio_proxy.c \
        tests/zephyr/peripheral/CMakeLists.txt tests/zephyr/peripheral/testcase.yaml \
        tests/zephyr/peripheral/prj_cc3501e_proxy.conf
git commit -m "fix(gpio): give the platform backend a real owner handle on the delegated path

The CC3501E proxy passed &s->inner -- a proxy_side_t member -- to the platform
z_open, which recovered an owner with CONTAINER_OF(st, struct alp_gpio, state).
Since state is the first member, owner became the sidecar reinterpreted as a
larger handle, and _isr_thunk read cb from past the end of the slot: a function
pointer built from the next _sides[] entry, called in ISR context.

Add alp_gpio_ops_t::open_delegated, which takes the owner explicitly. The proxy
recovers the genuine owner from its own state argument and passes it down. The
plain z_open now routes through the same body."
```

- [ ] **Step 12: Bench verification — E1M-AEN801 (BLOCKING before merge)**

`native_sim` proves the pointer is right; it does not prove the interrupt path works on silicon. Reserve the bench and run one of `examples/aen/aen-cc3501e-gpio`, `examples/aen/aen-cc3501e-bringup`, or `examples/aen/aen-cc3501e-companion-tour` — all three set `CONFIG_ALP_SDK_GPIO_CC3501E_PROXY=y`. Drive an edge on a **delegated** (non-routed) pin and confirm the registered callback fires with the correct handle. Capture the serial log and paste it into the PR's Test-plan section. Do not merge on a green `native_sim` alone.

---

## Task 3: Stop leaving a stack context registered with the Bluetooth host

**Issue: #1620** (`release-blocker`, milestone `v0.17.0`)

**Files:**
- Modify: `src/backends/ble/zephyr_drv.c:252-255` (the incorrect comment), `:277-308` (`ble_read_cb`), `:725-742` (`z_gatt_read`), `:768-785` (`z_gatt_write`)
- Test: `tests/zephyr/peripheral/src/ble_gatt.c`
- **`needs-silicon`: E1M-AEN801.**

**Interfaces:**
- Consumes: nothing from Tasks 1 or 2.
- Produces: nothing other tasks rely on.

**The defect.** The GATT read context is an automatic object whose `.params` is handed to the Bluetooth host, and the timeout path returns without cancelling:

```c
/* src/backends/ble/zephyr_drv.c:739-742 */
	bt_gatt_read(c->bt, &ctx.params);
	if (k_sem_take(&ctx.done, K_MSEC(timeout_ms)) != 0) return ALP_ERR_TIMEOUT;
	return ctx.result;
```

Zephyr offers no cancel for `bt_gatt_read`; `params` must stay valid until the callback fires or the connection drops. When the peer's Read Response finally arrives on the BT RX thread, `ble_read_cb` (`:277-308`) casts `params` back to the dead frame and writes through it — `memcpy(ctx->out, data, n)`, `*ctx->out_len = n`, `ctx->result = ALP_OK`, `k_sem_give(&ctx->done)` — into stack that has been returned from and reused, and gives a semaphore in recycled memory, which corrupts the kernel wait queue.

The comment at `:252-255` asserts the opposite and must be corrected in the same commit — it is what makes the bug look intentional to the next reader:

> "the ctx must remain valid until then, which the `k_sem_take()` call below guarantees for an on-stack instance"

A `k_sem_take` with a **finite** `K_MSEC(timeout_ms)` guarantees no such thing. A slow or unresponsive peer is normal BLE, not an exotic fault.

`z_gatt_write` at `:768-785` has the identical shape with `struct ble_write_ctx` and must be fixed in the same change.

**Fix shape.** Move both contexts into the per-connection `struct ble_conn_be` (pool-owned, lifetime bound to the handle rather than to the calling frame) and add a per-conn *read-in-flight* / *write-in-flight* flag so a second operation cannot reuse the slot until the previous callback has fired. On timeout, return `ALP_ERR_TIMEOUT` but leave the flag set and the context live; clear it in the callback. A subsequent operation on a connection with an in-flight slot returns `ALP_ERR_BUSY`.

Note the file already flags these paths BENCH-UNVERIFIED against issue #480 at `:718-721` — the defect is nonetheless visible statically, and that note explains why it survived.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b fix/ble-gatt-ctx-lifetime origin/dev
```

- [ ] **Step 2: Read the surrounding code before changing anything**

```bash
sed -n '240,320p' src/backends/ble/zephyr_drv.c   # the comment + both callbacks
sed -n '700,800p' src/backends/ble/zephyr_drv.c   # z_gatt_read and z_gatt_write
grep -n "struct ble_conn_be" src/backends/ble/zephyr_drv.c
```

Write down the exact current shape of `struct ble_conn_be`, `struct ble_read_ctx`, and `struct ble_write_ctx` before editing — the steps below add members to the first and relocate the other two.

- [ ] **Step 3: Write the failing test**

Create `tests/zephyr/peripheral/src/ble_gatt.c`. On `native_sim` there is no peer, so the reachable assertion is the **in-flight guard**, which is the observable half of the fix:

```c
#include <zephyr/ztest.h>
#include "alp/ble.h"
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_ble_gatt_read_timeout_leaves_slot_busy)
{
	/* Without a peer, the read must time out rather than block forever, and
	 * the connection's read slot must stay claimed until the (never-arriving)
	 * response clears it -- proving the ctx is pool-owned, not stack-owned. */
	ztest_test_skip(); /* requires a connected peer; see the bench step */
}
```

**Note for the implementer:** this test is a deliberate placeholder-with-a-skip, not a placeholder-with-a-TODO — the guard cannot be exercised without a peer. The real evidence for this task is the bench log in Step 8. If, while reading the code in Step 2, you find the BLE backend has an existing emulated-peer or mock transport under `tests/`, replace the skip with a real assertion against it and say so in the PR.

- [ ] **Step 4: Move the contexts into the connection pool**

In `src/backends/ble/zephyr_drv.c`, add to `struct ble_conn_be`:

```c
	struct ble_read_ctx  read_ctx;
	struct ble_write_ctx write_ctx;
	bool                 read_in_flight;
	bool                 write_in_flight;
```

Both context structs must be declared above `struct ble_conn_be` for this to compile — move their definitions up if they currently sit below.

- [ ] **Step 5: Claim, use, and guard the slot in `z_gatt_read`**

Replace the automatic `ctx` with the pool-owned one and refuse a concurrent second read:

```c
	if (c->read_in_flight) return ALP_ERR_BUSY;
	struct ble_read_ctx *ctx = &c->read_ctx;
	memset(ctx, 0, sizeof(*ctx));
	k_sem_init(&ctx->done, 0, 1);
	ctx->out     = out;
	ctx->out_len = out_len;
	ctx->owner   = c;          /* so the callback can clear the flag */
	c->read_in_flight = true;

	int err = bt_gatt_read(c->bt, &ctx->params);
	if (err != 0) {
		c->read_in_flight = false;
		return _errno_to_alp(err);
	}
	if (k_sem_take(&ctx->done, K_MSEC(timeout_ms)) != 0) {
		/* The ATT procedure is still outstanding and Zephyr cannot cancel it.
		 * Leave the ctx and the flag live: ble_read_cb still owns them and
		 * will clear the flag when the late response arrives. */
		return ALP_ERR_TIMEOUT;
	}
	c->read_in_flight = false;
	return ctx->result;
```

- [ ] **Step 6: Clear the flag in the callback**

In `ble_read_cb` (`:277-308`), after the existing result handling, clear the owning connection's flag so the slot becomes reusable once the late response lands:

```c
	struct ble_read_ctx *ctx = (struct ble_read_ctx *)params;
	/* ... existing memcpy / out_len / result handling, unchanged ... */
	if (ctx->owner != NULL) {
		ctx->owner->read_in_flight = false;
	}
	k_sem_give(&ctx->done);
```

- [ ] **Step 7: Repeat Steps 4-6 for `z_gatt_write`**

`z_gatt_write` at `:768-785` and its `ble_write_cb` have the identical shape with `struct ble_write_ctx` and `write_in_flight`. Do not skip it — it is the same defect and shipping only half the fix leaves a live corruption path.

- [ ] **Step 8: Correct the false comment**

`src/backends/ble/zephyr_drv.c:252-255` currently claims `k_sem_take()` guarantees on-stack ctx validity. Replace it with what is now true — that both contexts are pool-owned because a finite `k_sem_take` timeout returns while the ATT procedure is still outstanding, and Zephyr provides no cancel. Leaving the old comment is how this bug gets reintroduced.

- [ ] **Step 9: Audit for siblings**

Any other place in this backend that hands a stack address to the BT host across a finite wait is the same defect:

```bash
grep -n "k_sem_take(&ctx\|bt_gatt_\|struct bt_gatt_.*_params" src/backends/ble/zephyr_drv.c
```

Check the discovery, subscribe, and notify paths in particular.

- [ ] **Step 10: Format and run the full gate set**

```bash
clang-format -i src/backends/ble/zephyr_drv.c tests/zephyr/peripheral/src/ble_gatt.c
git diff --exit-code
bash scripts/test-all.sh --target dev
```

- [ ] **Step 11: Commit**

```bash
git add src/backends/ble/zephyr_drv.c tests/zephyr/peripheral/src/ble_gatt.c \
        tests/zephyr/peripheral/CMakeLists.txt
git commit -m "fix(ble): pool-own the GATT read/write contexts instead of leaving stack frames registered

z_gatt_read and z_gatt_write handed an automatic ctx to bt_gatt_read/write and
returned on a finite k_sem_take timeout without cancelling -- which Zephyr
cannot do. A late ATT response then memcpy'd into the returned-from frame and
gave a semaphore in recycled memory, corrupting the kernel wait queue. A slow
peer is normal BLE.

Move both contexts into struct ble_conn_be, add read_in_flight/write_in_flight
so a second op returns ALP_ERR_BUSY until the callback clears the slot, and
correct the comment that claimed k_sem_take guaranteed on-stack validity."
```

- [ ] **Step 12: Bench verification — E1M-AEN801 (BLOCKING before merge)**

The reachable failure needs a real peer. On the bench: connect to a peer, issue `alp_ble_gatt_read` against an attribute with a short `timeout_ms`, and induce a late response — either by choosing a peer that responds slowly or by moving the peer to the edge of range. Confirm the near side returns `ALP_ERR_TIMEOUT`, that a second read on the same connection returns `ALP_ERR_BUSY` rather than corrupting anything, and that the late response is absorbed without a fault. Capture the serial log into the PR. This is the evidence Step 3's skipped test cannot provide.

---

## Opening the three PRs

One PR per task, all `--base dev`. Follow `alp-lab:opening-github-prs-and-issues` — fill `.github/PULL_REQUEST_TEMPLATE.md` completely, and put `Closes #N` on its own for each linked issue (a comma-separated list closes only the first).

Each PR closes exactly one issue, on its own line:
`Closes #1619.` (Task 1) / `Closes #1618.` (Task 2) / `Closes #1620.` (Task 3).
Never `Closes #1618, #1619, #1620` — a comma-separated list closes only the first.

Labels for all three: `bug`, `area:drivers`. Add `aen` to Tasks 2 and 3 (both are AEN-specific paths), and `needs-silicon` to Tasks 2 and 3. Task 1 is portable — no SoM label.

Milestone: `v0.17.0` for all three.

**Do not merge ANY of the three on a green `native_sim` run.** Tasks 2 and 3 were always
bench-gated; Task 1 turned out to be too (see the correction at the top). All three
batch into one E1M-AEN801 reservation.

Task 1's bench step: on real silicon with an I2S device present, open a TX handle with
`block_frames = 256`, `channels = 2`, `word_bits = 16` (a 1024-byte block) and call
`alp_i2s_write(h, buf, 2048u, 100u)`. Confirm it returns `ALP_ERR_OUT_OF_RANGE` and that
`test_i2s_write_rejects_oversize_block` now runs rather than skipping. Capture the log.
