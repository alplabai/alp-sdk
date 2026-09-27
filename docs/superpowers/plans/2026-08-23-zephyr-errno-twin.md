# `alp_status_from_zephyr_errno()` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Issue: #1638** (`enhancement`, `area:drivers`, `area:portability`, milestone `Backlog`)

**Goal:** Ship the negative-errno counterpart to `alp_status_from_posix_errno()` and retire the 27 hand-rolled Zephyr switches that exist because it was never written.

**Architecture:** One new `static inline` in `src/common/alp_errno.h`, then 27 call sites collapse from a 16-26 line switch to a 3-6 line delegation. **This is not a pure refactor — it changes observable behaviour at up to 19 sites, deliberately.** Task 1 settles the one design question that decides all of it and lands the function with tests. Tasks 2-4 migrate the sites in three behaviour-classified batches so a reviewer can see which batch changes what.

**Tech Stack:** C (clang-format 22.x, tabs), Zephyr, ztest under `tests/unit/`.

**Spec:** `docs/superpowers/plans/2026-08-23-post-audit-hardening-campaign.md` — read its **Global Constraints** and **Verification infrastructure** sections first.

## Global Constraints

- Base branch is `dev`. Verify with `git merge-base HEAD origin/dev`. Never `--base main`.
- Branch from an up-to-date `origin/dev`. **Campaign Step 0 (the 280 staged files) must be resolved first.**
- `bash scripts/test-all.sh --target dev` green before `gh pr create`.
- clang-format **22.x** on every changed `.c`/`.h`.
- After `git merge origin/dev`, run `python3 scripts/gen_catalog.py` and commit the result.
- No AI attribution anywhere.
- **No ABI impact.** `src/common/alp_errno.h` is internal-only: `CMakeLists.txt:392-393` installs only `include/alp` (plus `include/alp/chips` at `:240-241`), `src/common/` appears in no `install()` rule, no public header includes it, and `docs/abi/*.json` contains no `alp_errno` reference. No ABI-snapshot regen is needed for the new function.

---

## The measured starting state

Inventoried at commit `8533874b`. **27 sites, not the 28 the campaign index estimated.** Every one is `static alp_status_t {_errno_to_alp,errno_to_alp}(int err)`, switches on a **negative** Zephyr errno, and delegates to nothing. None of the 27 currently includes `src/common/alp_errno.h`.

The baseline they should be delegating to:

```c
/* src/common/alp_errno.h:52-80 */
static inline alp_status_t alp_status_from_posix_errno(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case EINVAL:
		return ALP_ERR_INVAL;
	case EBUSY:
	case EAGAIN:
		return ALP_ERR_BUSY;
	case ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case ENOMEM:
		return ALP_ERR_NOMEM;
	case ENOENT:
	case ENODEV:
	case ENXIO:
		return ALP_ERR_NOT_READY;
	case ENOTSUP:
#if defined(EOPNOTSUPP) && (EOPNOTSUPP != ENOTSUP)
	case EOPNOTSUPP:
#endif
	case ENOSYS:
	case ENOTTY:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}
```

and the header comment that has been describing this exact gap:

```
/* src/common/alp_errno.h:19-21 */
 * Positive-POSIX-errno domain ONLY.  Do not pass a negative Zephyr
 * errno here (that is a distinct domain sign-wise; a Zephyr baseline
 * mapper is future work tracked by #630's Zephyr-side follow-up).
```

### The measured drift

| errno | baseline says | what the 27 sites actually do |
|---|---|---|
| `-EBUSY` | `ALP_ERR_BUSY` | **all 27 agree** — the only consistent arm in the tree |
| `-EAGAIN` | `ALP_ERR_BUSY` | **0 of 27 agree.** 16 → `ALP_ERR_TIMEOUT`; 11 have no arm → `ALP_ERR_IO` |
| `-ETIMEDOUT` | `ALP_ERR_TIMEOUT` | 19 agree; 8 have no arm → `ALP_ERR_IO` |
| `-ENODEV` / `-ENOENT` | `ALP_ERR_NOT_READY` | present in **2** sites only (both storage backends) |
| `-ENOMEM` | `ALP_ERR_NOMEM` | present in 9 sites |
| `-ENOSPC` | (falls to `ALP_ERR_IO`) | present in 2 sites, both → `ALP_ERR_NOMEM` |
| `-ENXIO`, `-EPERM`, `-EACCES`, `-EOVERFLOW` | mapped / `ALP_ERR_IO` | **appear in no Zephyr switch at all** |

**Eight sites are structurally incapable of ever returning `ALP_ERR_TIMEOUT`:** `dac/zephyr_drv.c:111`, `wdt/zephyr_drv.c:33`, `counter/zephyr_drv.c:35`, `qenc/zephyr_drv.c:35`, `pwm/zephyr_drv.c:93`, `rtc/zephyr_drv.c:34` (no `-ETIMEDOUT` **and** no `-EIO`), plus `gpio/zephyr_drv.c:130` and `storage/zephyr_littlefs.c:80` (have `-EIO`, still no `-ETIMEDOUT` or `-EAGAIN`).

### The one real documented-contract violation

Checked honestly, and most of the suspicion did not hold: **none of the six TIMEOUT-incapable headers document `ALP_ERR_TIMEOUT`** in their `@return` lists, so there is no contract violation there. State that as the negative result it is rather than implying a wider breach.

The genuine one is storage:

```
/* include/alp/storage.h:142-152, alp_storage_open() */
 * @return Open handle, or NULL with `alp_last_error()` set to one of
...
 *         failure (ALP_ERR_NOT_READY / ALP_ERR_BUSY /
 *         ALP_ERR_TIMEOUT / ALP_ERR_IO / ALP_ERR_OUT_OF_RANGE)
 *         translated from the underlying flash-area / littlefs
 *         error.
```

`src/backends/storage/zephyr_littlefs.c:80` has neither a `-EAGAIN` nor a `-ETIMEDOUT` arm, so a littlefs-backed `alp_storage_open()` can never produce the `ALP_ERR_TIMEOUT` its own doxygen promises. The flash half is fine — `storage/zephyr_flash.c:36` has the arm. One header, one of two backends, contradicting itself.

### The working precedent — and it is the *only* one

```c
/* src/backends/can/yocto_drv.c:178-190 */
static const alp_errno_override_t _can_errno_overrides[] = {
	{ EAGAIN, ALP_ERR_TIMEOUT },
	{ ENOPROTOOPT, ALP_ERR_NOSUPPORT },
};

static alp_status_t _errno_to_alp(int err)
{
	return alp_status_from_posix_errno_ex(
	    err, _can_errno_overrides, sizeof(_can_errno_overrides) / sizeof(_can_errno_overrides[0]));
}
```

`grep -rn alp_status_from_posix_errno_ex src` returns exactly two hits: the definition at `src/common/alp_errno.h:107` and this one call site. **`can/yocto_drv.c` is the sole consumer of the override form anywhere in the tree.** Note also that it overrides `EAGAIN → ALP_ERR_TIMEOUT` *against* the baseline, with a written rationale — that is a load-bearing precedent for Task 1's decision.

---

## Task 1: Decide `-EAGAIN`, then write the function

**Files:**
- Modify: `src/common/alp_errno.h`
- Test: `tests/unit/errno_mapping/` (create)

**Interfaces:**
- Produces: `alp_status_t alp_status_from_zephyr_errno(int err)` and `alp_status_t alp_status_from_zephyr_errno_ex(int err, const alp_errno_override_t *overrides, size_t n_overrides)`, both `static inline` in `src/common/alp_errno.h`. Tasks 2-4 call these; the names and signatures below are fixed.

**The decision that governs the whole plan.** The Zephyr twin must map `-EAGAIN`, and there is no option that is simultaneously behaviour-preserving and consistent with the POSIX baseline:

- Map it to `ALP_ERR_BUSY` (agreeing with the baseline) → **changes behaviour at the 16 sites that return `ALP_ERR_TIMEOUT` today**, including every camera, i2s, audio and RPC path where `-EAGAIN` genuinely means "the transfer did not complete in time".
- Map it to `ALP_ERR_TIMEOUT` → **disagrees with the POSIX baseline for the same spelling**, which is uncomfortably close to the drift this issue exists to remove.

**Decision: `-EAGAIN → ALP_ERR_TIMEOUT` in the Zephyr baseline, documented as a deliberate cross-domain divergence.** Three reasons, in order of weight:

1. It is what 16 of the 27 sites already do, so the migration preserves behaviour where behaviour exists rather than silently rewriting 16 drivers.
2. The two domains genuinely differ. In Zephyr driver APIs (`i2s_read`, `spi_transceive`, `k_sem_take`-backed paths) `-EAGAIN` is returned for a deadline that expired. In POSIX socket/file APIs `EAGAIN` means "would block, retry now" — a busy signal.
3. The tree already ratified exactly this reading on the POSIX side: `can/yocto_drv.c:178-180` overrides `EAGAIN → ALP_ERR_TIMEOUT` with the written rationale that it "should be treated like a deadline rather than an immediate bus-busy signal".

**This means the migration is not behaviour-neutral, and the plan must not pretend otherwise.** Net effect across the 27 sites:

| batch | sites | behaviour change |
|---|---|---|
| A — already match the twin exactly | 16 | none |
| B — gain arms they lack | 11 | `-EAGAIN` / `-ETIMEDOUT` stop returning `ALP_ERR_IO`; `-ENODEV`/`-ENOENT`/`-ENOMEM`/`-ENOSYS` become mapped |
| C — carry a distinctive arm the baseline lacks | 2 | none, via an override table |

Every change in batch B is a strictly-more-correct answer replacing `ALP_ERR_IO`. None of them narrows what a caller can receive; all of them widen it, which is why Task 3 carries the migration's only real risk and gets its own test.

- [ ] **Step 1: Create the branch**

```bash
git fetch origin
git checkout -b feat/1638-zephyr-errno-twin origin/dev
```

- [ ] **Step 2: Write the failing test**

Create `tests/unit/errno_mapping/` following the `tests/unit/status_strings/` layout (`CMakeLists.txt` with `find_package(Zephyr REQUIRED)`, `testcase.yaml` with `platform_allow: [native_sim, native_sim/native/64]`, `src/main.c`):

```c
#include <errno.h>

#include <zephyr/ztest.h>

#include "common/alp_errno.h"

ZTEST(alp_errno_mapping, test_zephyr_baseline_arms)
{
	zassert_equal(alp_status_from_zephyr_errno(0), ALP_OK);
	zassert_equal(alp_status_from_zephyr_errno(-EINVAL), ALP_ERR_INVAL);
	zassert_equal(alp_status_from_zephyr_errno(-EBUSY), ALP_ERR_BUSY);
	zassert_equal(alp_status_from_zephyr_errno(-ETIMEDOUT), ALP_ERR_TIMEOUT);
	zassert_equal(alp_status_from_zephyr_errno(-ENOMEM), ALP_ERR_NOMEM);
	zassert_equal(alp_status_from_zephyr_errno(-ENODEV), ALP_ERR_NOT_READY);
	zassert_equal(alp_status_from_zephyr_errno(-ENOENT), ALP_ERR_NOT_READY);
	zassert_equal(alp_status_from_zephyr_errno(-ENXIO), ALP_ERR_NOT_READY);
	zassert_equal(alp_status_from_zephyr_errno(-ENOTSUP), ALP_ERR_NOSUPPORT);
	zassert_equal(alp_status_from_zephyr_errno(-ENOSYS), ALP_ERR_NOSUPPORT);
	zassert_equal(alp_status_from_zephyr_errno(-ERANGE), ALP_ERR_OUT_OF_RANGE);
	zassert_equal(alp_status_from_zephyr_errno(-ENOSPC), ALP_ERR_NOMEM);
	zassert_equal(alp_status_from_zephyr_errno(-EIO), ALP_ERR_IO);
	zassert_equal(alp_status_from_zephyr_errno(-EPIPE), ALP_ERR_IO, "unmapped falls to IO");
}

/* The deliberate divergence from the POSIX baseline. If this test is ever
 * "fixed" to agree with alp_status_from_posix_errno(), read the rationale in
 * alp_errno.h first -- 16 Zephyr backends depend on this answer. */
ZTEST(alp_errno_mapping, test_zephyr_eagain_is_timeout_not_busy)
{
	zassert_equal(alp_status_from_zephyr_errno(-EAGAIN), ALP_ERR_TIMEOUT);
	zassert_equal(alp_status_from_posix_errno(EAGAIN), ALP_ERR_BUSY,
	              "the POSIX baseline must be left alone by this change");
}

ZTEST(alp_errno_mapping, test_zephyr_rejects_positive_errno)
{
	/* Positive input is the wrong domain and must not be silently mapped. */
	zassert_equal(alp_status_from_zephyr_errno(EINVAL), ALP_ERR_IO);
}

ZTEST(alp_errno_mapping, test_zephyr_override_form)
{
	static const alp_errno_override_t ov[] = {
		{ -ENOSPC, ALP_ERR_OUT_OF_RANGE },
	};
	zassert_equal(alp_status_from_zephyr_errno_ex(-ENOSPC, ov, ARRAY_SIZE(ov)),
	              ALP_ERR_OUT_OF_RANGE, "an override must win over the baseline");
	zassert_equal(alp_status_from_zephyr_errno_ex(-EBUSY, ov, ARRAY_SIZE(ov)), ALP_ERR_BUSY,
	              "a non-overridden arm must fall through to the baseline");
}

ZTEST_SUITE(alp_errno_mapping, NULL, NULL, NULL, NULL, NULL);
```

Note `test_zephyr_rejects_positive_errno`: the whole reason the two functions exist separately is that the domains differ by sign. A positive value handed to the Zephyr mapper is a caller bug, and it must land on `ALP_ERR_IO` rather than accidentally matching a case arm.

- [ ] **Step 3: Run the test and confirm it FAILS**

```bash
west twister -p native_sim/native/64 -T tests/unit/errno_mapping --no-clean -v
```

Expected: build failure — `implicit declaration of function 'alp_status_from_zephyr_errno'`.

- [ ] **Step 4: Write the function**

Add to `src/common/alp_errno.h`, after the existing `_ex` form. Negate and delegate rather than restating the table — a second parallel switch reintroduces exactly the drift being removed, and there is one arm that must *not* delegate:

```c
/**
 * @brief Map a NEGATIVE Zephyr errno to an @ref alp_status_t.
 *
 * The Zephyr driver APIs return `-EXXX`; the POSIX baseline above takes
 * `+EXXX`.  This is the negative-domain twin, and it delegates to that
 * baseline so the two cannot drift apart arm-by-arm (issue #1638; the
 * "Zephyr baseline mapper is future work" note at the top of this file).
 *
 * @par One deliberate divergence: -EAGAIN
 *      This returns @ref ALP_ERR_TIMEOUT where the POSIX baseline returns
 *      @ref ALP_ERR_BUSY.  In Zephyr driver APIs `-EAGAIN` is returned for
 *      an expired deadline (i2s_read, spi_transceive, k_sem_take-backed
 *      paths); in POSIX socket/file APIs `EAGAIN` means "would block, retry
 *      now".  Sixteen of the twenty-seven backends this replaced already
 *      answered TIMEOUT, and src/backends/can/yocto_drv.c overrides the
 *      POSIX baseline the same way for the same reason.  Do not "harmonise"
 *      these two without reading that.
 *
 * @param[in] err  Negative Zephyr errno, or 0.  A POSITIVE value is the
 *                 wrong domain and yields @ref ALP_ERR_IO -- it is not
 *                 silently treated as POSIX.
 * @return The mapped status.
 */
static inline alp_status_t alp_status_from_zephyr_errno(int err)
{
	if (err == 0) return ALP_OK;
	if (err > 0) return ALP_ERR_IO; /* wrong domain -- see @param */
	if (err == -EAGAIN) return ALP_ERR_TIMEOUT; /* see @par above */
	if (err == -ENOSPC) return ALP_ERR_NOMEM;   /* no baseline arm; out of store */
	if (err == -ERANGE) return ALP_ERR_OUT_OF_RANGE;
	return alp_status_from_posix_errno(-err);
}

/**
 * @brief @ref alp_status_from_zephyr_errno with per-backend overrides.
 *
 * Overrides are matched on the NEGATIVE value (e.g. `{ -ENOSPC, ... }`).
 *
 * @param[in] err          Negative Zephyr errno, or 0.
 * @param[in] overrides    Table searched before the baseline; may be NULL
 *                         when @p n_overrides is 0.
 * @param[in] n_overrides  Entries in @p overrides.
 * @return The mapped status.
 */
static inline alp_status_t
alp_status_from_zephyr_errno_ex(int err, const alp_errno_override_t *overrides, size_t n_overrides)
{
	for (size_t i = 0; i < n_overrides; ++i) {
		if (overrides[i].err == err) return overrides[i].status;
	}
	return alp_status_from_zephyr_errno(err);
}
```

Three arms are handled locally rather than by delegation, each for a stated reason: `-EAGAIN` is the documented divergence; `-ENOSPC` and `-ERANGE` have no baseline arm at all (they would fall to `ALP_ERR_IO`) but appear in the sites being migrated and must keep their meaning.

- [ ] **Step 5: Correct the stale header comment**

`src/common/alp_errno.h:19-21` says a Zephyr mapper "is future work". It is no longer future work. Rewrite it to point at the new function and at the `-EAGAIN` divergence. Leaving it is how the next person concludes the twin still does not exist and writes a 28th switch.

- [ ] **Step 6: Run the tests and confirm they PASS**

```bash
west twister -p native_sim/native/64 -T tests/unit/errno_mapping --no-clean -v
```

- [ ] **Step 7: Format, gate, commit**

```bash
clang-format -i src/common/alp_errno.h tests/unit/errno_mapping/src/main.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git add src/common/alp_errno.h tests/unit/errno_mapping/
git commit -m "feat(common): add alp_status_from_zephyr_errno(), the negative-errno twin

alp_errno.h has documented since #630 that its baseline is positive-POSIX only
and that a Zephyr mapper was future work. In the absence of one, 27 backends
hand-rolled their own switch and drifted: -EAGAIN reaches ALP_ERR_TIMEOUT at 16
sites and ALP_ERR_IO at 11, and 8 sites cannot return ALP_ERR_TIMEOUT at all.

The twin negates and delegates so the two domains cannot drift arm-by-arm.
-EAGAIN deliberately maps to ALP_ERR_TIMEOUT rather than the baseline's
ALP_ERR_BUSY: in Zephyr driver APIs it signals an expired deadline, which is
what 16 of the 27 backends already answered and what can/yocto_drv.c already
overrides the POSIX baseline to do."
```

---

## Task 2: Migrate batch A — the 16 no-behaviour-change sites

**Files:** the 16 sites whose arms the twin already reproduces exactly.

**Interfaces:** consumes `alp_status_from_zephyr_errno()` from Task 1.

These sites map `-EAGAIN` and `-ETIMEDOUT` to `ALP_ERR_TIMEOUT` and otherwise match the baseline, so delegation is observably identical. **Migrate these first**: it is the largest batch, it is provably a no-op, and getting it reviewed and merged separately means the batches that *do* change behaviour arrive small and legible.

- [ ] **Step 1: Migrate one site and diff the behaviour by hand**

Start with `src/backends/i2c/zephyr_drv.c:43`. Replace the switch body with:

```c
#include "common/alp_errno.h"

static alp_status_t _errno_to_alp(int err)
{
	return alp_status_from_zephyr_errno(err);
}
```

Before deleting the old switch, write its arms down and check each against Task 1's test list. If any arm disagrees, the site belongs in batch B or C — move it and say so.

- [ ] **Step 2: Migrate the remaining 15**

- [ ] `src/backends/display/zephyr_drv.c:87`
- [ ] `src/backends/can/zephyr_drv.c:93`
- [ ] `src/backends/ble/zephyr_drv.c:139`
- [ ] `src/backends/wifi/zephyr_drv.c:66`
- [ ] `src/backends/camera/alif_isp_pico.c:106`
- [ ] `src/backends/camera/v2n_n44_isp.c:108`
- [ ] `src/backends/camera/zephyr_video.c:110`
- [ ] `src/backends/audio/zephyr_drv.c:120`
- [ ] `src/backends/i2s/zephyr_drv.c:84`
- [ ] `src/backends/usb/zephyr_drv.c:84`
- [ ] `src/backends/mqtt/zephyr_drv.c:119`
- [ ] `src/backends/i3c/zephyr_drv.c:68`
- [ ] `src/backends/rpc/zephyr_drv.c:301`
- [ ] `src/backends/mproc/zephyr_drv.c:188`
- [ ] `src/backends/uart/zephyr_drv.c:75` — **check this one carefully.** It has `-ETIMEDOUT` but *no* `-EAGAIN` arm, so it is a batch B site by the letter of the inventory. It is listed here because `uart_poll_in` returning `-EAGAIN` ("no character available") mapping to `ALP_ERR_TIMEOUT` is the behaviour `alp_uart_read`'s documented `timeout_ms == 0` poll-once semantics (`include/alp/peripheral.h:911-917`) already promises. Confirm that reading, and if it does not hold, move it to batch B.

- [ ] **Step 3: Confirm the three camera sites really were identical**

`camera/alif_isp_pico.c`, `camera/v2n_n44_isp.c` and `camera/zephyr_video.c` were reported as byte-identical bodies. Verify before deleting all three:

```bash
for f in src/backends/camera/alif_isp_pico.c src/backends/camera/v2n_n44_isp.c src/backends/camera/zephyr_video.c; do
  echo "== $f"; sed -n '/_errno_to_alp/,/^}/p' "$f" | md5sum
done
```

Three identical hashes is the evidence. Different hashes means one drifted and needs individual reading.

- [ ] **Step 4: Format, gate, commit**

```bash
clang-format -i $(git diff --name-only --diff-filter=M | grep -E '\.(c|h)$')
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "refactor: delegate 16 Zephyr errno mappers to alp_status_from_zephyr_errno()

No behaviour change: all 16 already produced exactly what the twin produces.
Deletes ~330 lines of hand-copied switch."
```

---

## Task 3: Migrate batch B — the 11 sites that gain arms

**Files:** the 11 sites missing `-EAGAIN` and/or `-ETIMEDOUT`.

**Interfaces:** consumes `alp_status_from_zephyr_errno()` from Task 1.

**This batch carries the migration's only real risk and needs its own review.** Every one of these currently answers `ALP_ERR_IO` for errno values the twin maps properly. After migration a caller can receive `ALP_ERR_TIMEOUT`, `ALP_ERR_NOT_READY`, `ALP_ERR_NOMEM` or `ALP_ERR_NOSUPPORT` where it previously always saw `ALP_ERR_IO`.

That is strictly more correct, and it is still a behaviour change. Any caller doing `if (rc == ALP_ERR_IO)` on these classes changes meaning.

- [ ] **Step 1: Find the callers before changing anything**

```bash
grep -rn "ALP_ERR_IO" src/ examples/ tests/ --include=*.c --include=*.cpp --include=*.h
```

For each of the 11 classes, check whether any caller branches on `ALP_ERR_IO` from that class specifically. Record what you found in the PR body — "no caller branches on it" is a fine answer, but it has to be an answer, not an assumption.

- [ ] **Step 2: Migrate the six TIMEOUT-incapable sites**

These have neither `-ETIMEDOUT` nor `-EIO`:

- [ ] `src/backends/dac/zephyr_drv.c:111`
- [ ] `src/backends/wdt/zephyr_drv.c:33`
- [ ] `src/backends/counter/zephyr_drv.c:35`
- [ ] `src/backends/qenc/zephyr_drv.c:35`
- [ ] `src/backends/pwm/zephyr_drv.c:93`
- [ ] `src/backends/rtc/zephyr_drv.c:34`

**None of these six headers documents `ALP_ERR_TIMEOUT`,** so this is not fixing a contract violation — it is making a timeout reportable where the driver can produce one. Do not overclaim it in the commit message.

- [ ] **Step 3: Migrate `gpio` and `storage/zephyr_littlefs`**

- [ ] `src/backends/gpio/zephyr_drv.c:130` — has `-EIO`, lacks `-EAGAIN`/`-ETIMEDOUT`.
- [ ] `src/backends/storage/zephyr_littlefs.c:80` — **this one closes a real documented-contract violation.** `include/alp/storage.h:142-152` promises `alp_storage_open()` can return `ALP_ERR_TIMEOUT` "translated from the underlying flash-area / **littlefs** error", and this mapper cannot produce it. Its sibling `storage/zephyr_flash.c:36` already can. Call this out specifically in the PR — it is the strongest single justification in the whole issue.

  Note this site also maps `-ENOSPC → ALP_ERR_NOMEM` and `-ERANGE → ALP_ERR_OUT_OF_RANGE`; both are in the twin's local arms (Task 1 Step 4), so delegation preserves them. Verify that rather than assuming it.

- [ ] **Step 4: Migrate the remaining three**

- [ ] `src/backends/storage/zephyr_flash.c:36` — has `-ETIMEDOUT`, no `-EAGAIN`.
- [ ] `src/backends/spi/zephyr_drv.c:105` — has `-ETIMEDOUT`, no `-EAGAIN`.
- [ ] `src/backends/jpeg/alif_hantro.c:163` — has both, plus `-ENOBUFS → ALP_ERR_NOMEM` which the twin does **not** map. This one needs an override table; see Task 4. Move it there rather than losing the arm.

- [ ] **Step 5: Add a test pinning the newly-reachable statuses**

Extend `tests/unit/errno_mapping/src/main.c` with a test asserting that the classes in this batch can now produce `ALP_ERR_TIMEOUT` — not by calling hardware, but by asserting the mapper each class now uses is the twin. If that is not reachable from a unit test, say so and rely on Task 1's tests plus the code review; do not write a test that cannot fail.

- [ ] **Step 6: Format, gate, commit**

```bash
clang-format -i $(git diff --name-only --diff-filter=M | grep -E '\.(c|h)$')
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "fix: 11 Zephyr backends answered ALP_ERR_IO for mappable errno

These mappers lacked -EAGAIN and/or -ETIMEDOUT arms, so a timed-out transfer
surfaced as ALP_ERR_IO. Eight of them could never return ALP_ERR_TIMEOUT at all.

storage/zephyr_littlefs.c is a documented-contract violation: storage.h states
alp_storage_open() returns ALP_ERR_TIMEOUT translated from the underlying
littlefs error, and its mapper had no arm that could produce one.

Behaviour change: callers of these classes can now receive ALP_ERR_TIMEOUT,
ALP_ERR_NOT_READY, ALP_ERR_NOMEM or ALP_ERR_NOSUPPORT where they previously
always saw ALP_ERR_IO."
```

---

## Task 4: Migrate batch C — the sites needing an override table

**Files:** `src/backends/jpeg/alif_hantro.c:163`, plus any site Task 2 or 3 moved here.

**Interfaces:** consumes `alp_status_from_zephyr_errno_ex()` from Task 1.

`jpeg/alif_hantro.c` maps `-ENOBUFS → ALP_ERR_NOMEM`, which the twin does not carry (it is a JPEG-encoder-specific "output buffer too small" signal, not a general allocation failure). Use the override form, matching the CAN precedent's shape exactly:

```c
#include "common/alp_errno.h"

/* -ENOBUFS from the Hantro encoder means the caller's output buffer was too
 * small for the encoded frame -- an allocation-shaped failure specific to this
 * backend, with no arm in the shared baseline. */
static const alp_errno_override_t _hantro_errno_overrides[] = {
	{ -ENOBUFS, ALP_ERR_NOMEM },
};

static alp_status_t _errno_to_alp(int err)
{
	return alp_status_from_zephyr_errno_ex(err, _hantro_errno_overrides,
	                                       sizeof(_hantro_errno_overrides) /
	                                           sizeof(_hantro_errno_overrides[0]));
}
```

Note the override key is **negative** (`-ENOBUFS`), unlike the CAN table's positive `EAGAIN` — the two `_ex` forms match on the value in their own domain. Getting this wrong produces a table that silently never matches.

- [ ] **Step 1: Migrate, format, gate, commit**

```bash
clang-format -i src/backends/jpeg/alif_hantro.c
git diff --exit-code
bash scripts/test-all.sh --target dev
git commit -am "refactor(jpeg): delegate the Hantro errno mapper with a -ENOBUFS override"
```

- [ ] **Step 2: Prove the class is closed**

```bash
grep -rnE "static alp_status_t _?errno_to_alp\(int" src/
```

Expected after all four tasks: exactly **two** hits — `src/backends/can/yocto_drv.c:183` (the POSIX precedent, correctly delegating) and nothing else that owns a switch. Every other hit must be a one-line delegation. If a hand-rolled switch remains, it was missed; name it in the PR rather than leaving it.

---

## Deliberately out of scope

Six more mapper-shaped functions exist and are **not** part of this issue. Note them in the PR so the next reader does not think they were missed:

- **`se_rc_to_alp` — four byte-identical copies** at `src/backends/soc_info/alif_se.c:53`, `src/backends/security/se_cryptocell.c:145`, `src/backends/mproc/alif_se_boot.c:94`, `src/backends/power/alif_se_profile.c:57`. Each maps `0→ALP_OK, -EINVAL→ALP_ERR_INVAL, -EAGAIN→ALP_ERR_NOT_READY, -EBUSY→ALP_ERR_NOT_READY, default→ALP_ERR_IO`. This is the same disease in a different domain — a mixed negative-errno / positive-SE-firmware-code space — and it wants its own shared helper. **Note that these map `-EAGAIN → ALP_ERR_NOT_READY`, a third answer for the same errno**, which is worth recording even though fixing it is separate work. File it as a follow-up.
- `src/yocto/inference_drpai.cpp:152` `_drpai_errno_to_status` — positive POSIX with its own switch; belongs to the POSIX baseline's migration, not this one.
- `src/yocto/inference_ort.cpp:167` `_ort_errorcode_to_status` — maps `OrtErrorCode`, an entirely different enum. Not errno. Leave alone.

---

## Opening the PRs

Four PRs, all `--base dev`, in task order. Task 1 must merge before the others compile.

- Task 1: `Refs #1638.` Labels `enhancement`, `area:portability`.
- Task 2: `Refs #1638.` Labels `refactor`, `area:drivers`.
- Task 3: `Refs #1638.` Labels `bug`, `area:drivers`, `area:portability` — this is the batch that fixes the storage contract violation.
- Task 4: `Closes #1638.` Labels `refactor`, `area:drivers`.

They can ship as one PR if you prefer, but batch B changes observable behaviour and batch A does not — splitting is what lets a reviewer give the risky ~150 lines real attention instead of skimming them inside a 600-line diff.

**No bench time required.** Every change is a pure status-translation change with no hardware behaviour. Batch B's widened return set is worth a bench smoke on E1M-AEN801 if one is already scheduled for another plan, but it does not gate the merge.
