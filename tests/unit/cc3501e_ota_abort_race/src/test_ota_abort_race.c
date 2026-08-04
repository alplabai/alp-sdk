/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the cc3501e-bridge TI backend's OTA abort-vs-FINISH state
 * machine (issue #1123): cc3501e_hw_ota_abort() racing the deferred FINISH
 * work in cc3501e_hw_ota_pump() must never let a cancelled session install
 * or reboot into the image it was told to cancel -- and must never leave a
 * flash-committed image behind for cc3501e_hw_ota_promote() to boot later
 * (the review-round blocker: round 1 gated the two RAM writes but never
 * walked the slot itself back).
 *
 * cc3501e_hw_ti_ota.c calls TI's PSA-FWU flash API only through
 * cc3501e_hw_ti_ota_psa.h's plain-C seam (never <ti/utils/FWU/psa_fwu.h>
 * directly), so the REAL production state machine links here unmodified
 * against an in-memory mock of that seam -- no vendor SimpleLink SDK
 * needed on native_sim.  Mirrors gd32-bridge's tests/unit/gd32_bridge_ota
 * (weak-seam-override) pattern.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "cc3501e_hw.h"
#include "cc3501e_hw_ti_internal.h"
#include "cc3501e_hw_ti_ota_psa.h"
#include "alp/protocol/cc3501e.h"

/* ---- transport seam: this TU never touches real SPI hardware ---- */
void bridge_transport_spi_hw_reinit(void)
{
}

/* ---- deferred-reboot latch: normally defined in cc3501e_hw_ti.c, which
 * is NOT linked here (it owns unrelated CMD_RESET / tick machinery this
 * test doesn't need) -- the test supplies the storage instead. */
volatile bool   reply_drained;
volatile bool   ota_reboot_pending;
volatile int8_t ota_reboot_rc;

/* ---- PSA-FWU seam mock -------------------------------------------------
 *
 * Tracks call counts so a test can prove NOT ONLY "reboot_pending stayed
 * false" but "the vendor finish/install sequence never (or did) run", AND
 * "the slot was actually walked back" (g_cancel_calls / g_reject_calls /
 * g_clean_calls -- the blocker: round 1's mock didn't count these, which is
 * structurally why it couldn't see that ota_release_slot() was never
 * called). */
static unsigned g_start_calls;
static unsigned g_write_calls;
static unsigned g_finish_calls;
static unsigned g_install_calls;
static unsigned g_cancel_calls;
static unsigned g_reject_calls;
static unsigned g_clean_calls;
/* When true, the mock cc3501e_ota_psa_install()/_finish() calls
 * cc3501e_hw_ota_abort() from INSIDE itself, right before returning success
 * -- simulating the SPI dispatch context's ABORT landing at that exact
 * checkpoint: g_install_reentrant_abort for "after the vendor flash commit,
 * before ota_finish_step() publishes STAGED / arms the reboot latch";
 * g_finish_reentrant_abort for "after finish() (CANDIDATE), before
 * install() ever runs" (MAJOR 3 from review -- the narrower checkpoint
 * between the two vendor calls). */
static bool g_install_reentrant_abort;
static bool g_finish_reentrant_abort;

static void mock_reset(void)
{
	g_start_calls             = 0u;
	g_write_calls             = 0u;
	g_finish_calls            = 0u;
	g_install_calls           = 0u;
	g_cancel_calls            = 0u;
	g_reject_calls            = 0u;
	g_clean_calls             = 0u;
	g_install_reentrant_abort = false;
	g_finish_reentrant_abort  = false;
	ota_reboot_pending        = false;
	ota_reboot_rc             = 0;
	reply_drained             = true;
}

/* Zero just the PSA-FWU call counters, leaving ota_reboot_pending/
 * reply_drained/ota_reboot_rc untouched -- for a test that needs to isolate
 * ONE call's own call counts (e.g. an abort's walk-back) while still
 * checking whether that same call correctly disarms a latch a PRIOR,
 * already-completed step armed. */
static void reset_call_counts(void)
{
	g_start_calls   = 0u;
	g_write_calls   = 0u;
	g_finish_calls  = 0u;
	g_install_calls = 0u;
	g_cancel_calls  = 0u;
	g_reject_calls  = 0u;
	g_clean_calls   = 0u;
}

void cc3501e_ota_psa_init(void)
{
}

uint32_t cc3501e_ota_psa_manifest_size(void)
{
	return 4u; /* small + fixed so the tests can position ticks exactly */
}

bool cc3501e_ota_psa_query_primary(uint8_t slot, bool *out_primary)
{
	/* Slot 1 reads primary, slot 2 does not -- ota_do_begin() always
	 * resolves target = slot 2 without hitting the ambiguous-primary
	 * recovery branch. */
	*out_primary = (slot == CC3501E_OTA_PSA_SLOT_1);
	return true;
}

bool cc3501e_ota_psa_cancel(uint8_t slot)
{
	(void)slot;
	g_cancel_calls++;
	return true;
}

bool cc3501e_ota_psa_reject(void)
{
	g_reject_calls++;
	return true;
}

bool cc3501e_ota_psa_clean(uint8_t slot)
{
	(void)slot;
	g_clean_calls++;
	return true;
}

bool cc3501e_ota_psa_start(uint8_t slot, const uint8_t *manifest, uint32_t manifest_len)
{
	(void)slot;
	(void)manifest;
	(void)manifest_len;
	g_start_calls++;
	return true;
}

bool cc3501e_ota_psa_write(uint8_t slot, uint32_t offset, const uint8_t *data, uint32_t len)
{
	(void)slot;
	(void)offset;
	(void)data;
	(void)len;
	g_write_calls++;
	return true;
}

bool cc3501e_ota_psa_finish(uint8_t slot)
{
	(void)slot;
	g_finish_calls++;
	if (g_finish_reentrant_abort) {
		(void)cc3501e_hw_ota_abort();
	}
	return true;
}

bool cc3501e_ota_psa_install(void)
{
	g_install_calls++;
	if (g_install_reentrant_abort) {
		(void)cc3501e_hw_ota_abort();
	}
	return true;
}

/* ---- test helpers ------------------------------------------------------
 *
 * TEST_DATA_LEN=6 + the mocked 4-byte manifest = a 10-byte TEST_TOTAL_LEN
 * image.  6 < CC3501E_OTA_FINISH_FLASH_BLOCK (4096), so FINISH's WRITE
 * phase needs exactly ONE psa_fwu_write block, and that block's tick folds
 * the WRITE->INSTALL phase transition in with it -- so the whole FINISH
 * sequence takes exactly 3 cc3501e_hw_ota_pump() calls:
 *   1. START   -- cc3501e_ota_psa_start(), advances to WRITE
 *   2. WRITE   -- the one data block; finish_off reaches total_len in the
 *                 SAME call, so this call also advances to INSTALL
 *   3. INSTALL -- cc3501e_ota_psa_finish() + _install(), publishes STAGED
 *                 and arms ota_reboot_pending (unless abort intervened)
 * Chosen deliberately small + exact so the abort-injection tests below can
 * position an abort precisely between calls 2 and 3. */
#define TEST_DATA_LEN     6u
#define TEST_MANIFEST_LEN 4u /* == cc3501e_ota_psa_manifest_size() above */
#define TEST_TOTAL_LEN    (TEST_MANIFEST_LEN + TEST_DATA_LEN)
#define TEST_FINISH_STEPS 3u

static uint8_t ota_state_now(void)
{
	uint8_t state = 0xFFu;
	zassert_equal(cc3501e_hw_ota_status(&state, NULL, NULL), CC3501E_HW_OK);
	return state;
}

/* Force a clean IDLE session regardless of the previous test's leftover
 * state (ztest cases share one native_sim process -- no per-case re-link). */
static void reset_all(void)
{
	mock_reset();
	(void)cc3501e_hw_ota_abort(); /* synchronous when nothing is in flight */
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_IDLE, "must start each test from IDLE");
}

/* Drive BEGIN + a single WRITE covering the whole TEST_TOTAL_LEN image,
 * leaving state WRITING with a FINISH ready to submit. */
static void begin_and_write_all(void)
{
	uint8_t data[TEST_TOTAL_LEN];

	memset(data, 0xAB, sizeof data);
	zassert_equal(
	    cc3501e_hw_ota_begin(TEST_TOTAL_LEN), CC3501E_HW_BUSY, "BEGIN queues, not immediate");
	cc3501e_hw_ota_pump(); /* ota_do_begin() is not chunked -- one call completes it */
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_WRITING, "BEGIN must land in WRITING");
	zassert_equal(cc3501e_hw_ota_write(0u, data, sizeof data), CC3501E_HW_OK);
}

/* Drive a full BEGIN+WRITE+FINISH cycle to a clean STAGED completion (no
 * abort).  Used by the leak-isolation test as "a totally unrelated, later
 * session" that must be unaffected by an EARLIER session's abort. */
static void complete_one_clean_session(void)
{
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);
	for (unsigned i = 0u; i < TEST_FINISH_STEPS; i++) {
		cc3501e_hw_ota_pump();
	}
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_STAGED);
	zassert_equal(ota_reboot_pending, true);
}

ZTEST_SUITE(cc3501e_ota_abort_race, NULL, NULL, NULL, NULL, NULL);

/* Baseline / control: no abort at all -- FINISH must still complete, and
 * must take MORE THAN ONE pump() call to do it (proves chunking actually
 * chunks, not just a single call in disguise). */
ZTEST(cc3501e_ota_abort_race, test_finish_completes_without_abort)
{
	reset_all();
	begin_and_write_all();

	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY, "FINISH queues, not immediate");

	cc3501e_hw_ota_pump(); /* call 1: START */
	zassert_equal(g_start_calls, 1u);
	zassert_equal(g_install_calls, 0u, "must not reach INSTALL after only one call -- chunked");
	zassert_equal(ota_reboot_pending, false);

	for (unsigned i = 0u; i < TEST_FINISH_STEPS - 1u; i++) {
		cc3501e_hw_ota_pump();
	}

	zassert_equal(g_finish_calls, 1u);
	zassert_equal(g_install_calls, 1u);
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_STAGED, "clean FINISH must reach STAGED");
	zassert_equal(ota_reboot_pending, true, "clean FINISH must arm the swap-reboot");
}

/* Abort with nothing in flight: synchronous immediate clear (mirrors
 * gd32-bridge's CMD_OTA_ABORT -- no deferred work exists to race). */
ZTEST(cc3501e_ota_abort_race, test_abort_with_nothing_inflight_is_synchronous)
{
	reset_all();
	begin_and_write_all();
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_WRITING);

	zassert_equal(cc3501e_hw_ota_abort(), CC3501E_HW_OK);
	zassert_equal(
	    ota_state_now(), ALP_CC3501E_OTA_STATE_IDLE, "abort with no FINISH queued clears at once");
}

/* ABORT AFTER AN ALREADY-COMPLETED FINISH (no race at all: the session
 * reached STAGED cleanly, THEN the host calls ABORT).  cc3501e_hw_ota_abort()
 * takes the "nothing in flight" synchronous path here (op_rc is not
 * INFLIGHT), which -- per issue #1123's review -- must ALSO walk the slot
 * back and disarm the reboot latch, not just the racing-FINISH case. */
ZTEST(cc3501e_ota_abort_race, test_abort_after_completed_finish_releases_slot)
{
	reset_all();
	complete_one_clean_session();
	zassert_equal(ota_reboot_pending, true); /* precondition: FINISH really did arm it */

	reset_call_counts(); /* isolate this call's own cancel/reject/clean count */
	zassert_equal(cc3501e_hw_ota_abort(), CC3501E_HW_OK);

	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_IDLE);
	zassert_equal(ota_reboot_pending, false, "abort must disarm a latch an earlier FINISH armed");
	zassert_equal(g_reject_calls, 1u, "abort after STAGED must reject() the slot (#1123)");
	zassert_equal(g_clean_calls, 1u, "abort after STAGED must clean() the slot (#1123)");
}

/* THE #1123 REGRESSION -- abort lands after the WRITE phase has finished
 * streaming but BEFORE psa_fwu_finish()/install() ever run.  Must block
 * the flash-commit calls entirely, not just the state publish. */
ZTEST(cc3501e_ota_abort_race, test_abort_before_install_blocks_finish_and_install)
{
	reset_all();
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);

	/* Calls 1-2: START, WRITE (the one data block, folding the WRITE->INSTALL
	 * transition).  psa_fwu_finish()/install() have NOT run yet at this point. */
	for (unsigned i = 0u; i < TEST_FINISH_STEPS - 1u; i++) {
		cc3501e_hw_ota_pump();
	}
	zassert_equal(g_finish_calls, 0u, "must not have reached INSTALL yet");
	zassert_equal(g_install_calls, 0u);

	zassert_equal(cc3501e_hw_ota_abort(), CC3501E_HW_OK, "abort while FINISH is mid-flight");

	/* Let the pump settle (bounded, generous margin over the 1 call actually
	 * needed to unwind). */
	for (unsigned i = 0u; i < 8u; i++) {
		cc3501e_hw_ota_pump();
	}

	zassert_equal(g_finish_calls, 0u, "an aborted FINISH must never call psa_fwu_finish");
	zassert_equal(g_install_calls, 0u, "an aborted FINISH must never call psa_fwu_install");
	zassert_equal(
	    ota_reboot_pending, false, "aborted session must not arm the swap-reboot (#1123)");
	zassert_equal(ota_state_now(),
	              ALP_CC3501E_OTA_STATE_IDLE,
	              "aborted session must not read STAGED (#1123)");
}

/* THE TIGHTEST #1123 WINDOW -- abort lands AFTER psa_fwu_install() has
 * already committed the image to flash (the mock re-enters
 * cc3501e_hw_ota_abort() from inside cc3501e_ota_psa_install()).  The
 * flash write cannot be unwound at this point, but the publish it gates
 * -- ota.state and ota_reboot_pending -- must still never happen, AND
 * (the review-round blocker) the slot must actually be walked back:
 * reject() then clean(), not just the two RAM writes skipped. */
ZTEST(cc3501e_ota_abort_race, test_abort_racing_install_still_blocks_stage_and_reboot)
{
	reset_all();
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);
	cc3501e_hw_ota_pump(); /* call 1: START -- its OWN unconditional walk-back runs here */
	/* Isolate the INSTALL-triggered walk-back's own call count from BOTH
	 * ota_do_begin()'s and START's own (unrelated) walk-backs. */
	reset_call_counts();

	g_install_reentrant_abort = true;
	for (unsigned i = 0u; i < TEST_FINISH_STEPS - 1u; i++) {
		cc3501e_hw_ota_pump();
	}

	zassert_equal(g_install_calls, 1u, "the flash commit itself is NOT retroactively undoable");
	zassert_equal(
	    ota_reboot_pending, false, "a cancelled session must never arm the swap-reboot (#1123)");
	zassert_equal(ota_state_now(),
	              ALP_CC3501E_OTA_STATE_IDLE,
	              "a cancelled session must never read STAGED (#1123)");
	/* THE BLOCKER: round 1 gated only the two RAM writes above and left the
	 * flash-committed STAGED image standing -- cc3501e_hw_ota_promote() would
	 * still boot it.  These assertions are what actually prove the slot
	 * itself got walked back (ota_release_slot(): cancel+reject+clean); they
	 * FAIL against the round-1 fix (0 calls each) and PASS only once
	 * ota_finish_step()'s post-install checkpoint calls it. */
	zassert_equal(g_cancel_calls, 1u, "a STAGED-committed abort must cancel() the slot (#1123)");
	zassert_equal(g_reject_calls, 1u, "a STAGED-committed abort must reject() the slot (#1123)");
	zassert_equal(g_clean_calls, 1u, "a STAGED-committed abort must clean() the slot (#1123)");
}

/* Narrower version of the same blocker: abort lands between psa_fwu_finish()
 * (slot now CANDIDATE) and psa_fwu_install() -- MAJOR 3 from review.  Still
 * fully unwindable at this point (cancel() applies to CANDIDATE, unlike
 * post-install), so cc3501e_ota_psa_install() must never be called at all,
 * and the walk-back must still run (cancel()+clean(), matching the
 * CANDIDATE-state comment on ota_finish_step's mid-INSTALL checkpoint). */
ZTEST(cc3501e_ota_abort_race, test_abort_between_finish_and_install_never_installs)
{
	reset_all();
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);
	cc3501e_hw_ota_pump(); /* call 1: START -- its OWN unconditional walk-back runs here */
	/* Isolate the finish()-triggered walk-back's own call count from BOTH
	 * ota_do_begin()'s and START's own (unrelated) walk-backs. */
	reset_call_counts();

	g_finish_reentrant_abort = true;
	for (unsigned i = 0u; i < TEST_FINISH_STEPS - 1u; i++) {
		cc3501e_hw_ota_pump();
	}

	zassert_equal(g_finish_calls, 1u, "finish() itself still runs -- it's the call that races");
	zassert_equal(g_install_calls, 0u, "install() must never run once finish() saw the abort");
	zassert_equal(ota_reboot_pending, false);
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_IDLE);
	zassert_equal(g_cancel_calls, 1u, "CANDIDATE walk-back must cancel() the slot");
	zassert_equal(g_reject_calls, 1u, "CANDIDATE walk-back (ota_release_slot) also calls reject()");
	zassert_equal(g_clean_calls, 1u, "CANDIDATE walk-back must clean() the slot");
}

/* THE #1123-ROUND-2 LEAK -- a stale abort request must never cancel a LATER,
 * unrelated session.  Sequentially reproduces the structural property the
 * generation-counter fix provides (not the exact ISR-preemption timing that
 * CREATES a stale request, which a single-threaded harness cannot
 * manufacture): abort a genuinely in-flight FINISH, let it fully unwind to
 * IDLE, then run a completely separate, later session to full success and
 * confirm it is unaffected. */
ZTEST(cc3501e_ota_abort_race, test_stale_abort_does_not_leak_into_next_session)
{
	reset_all();

	/* Session 1: real, legitimate mid-flight abort (same shape as
	 * test_abort_before_install_blocks_finish_and_install). */
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);
	cc3501e_hw_ota_pump(); /* call 1: START only, stay mid-flight */
	zassert_equal(cc3501e_hw_ota_abort(), CC3501E_HW_OK);
	for (unsigned i = 0u; i < 8u; i++) {
		cc3501e_hw_ota_pump(); /* drain the unwind */
	}
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_IDLE, "session 1 must have unwound");

	/* Session 2: a totally separate, later BEGIN/WRITE/FINISH.  If the
	 * generation fix did not close the leak, this session would inherit
	 * session 1's stale cancellation and silently fail to reach STAGED. */
	mock_reset(); /* fresh call counts for session 2's own assertions */
	complete_one_clean_session();
	zassert_equal(
	    g_install_calls, 1u, "session 2 must reach INSTALL, unaffected by session 1's abort");
}
