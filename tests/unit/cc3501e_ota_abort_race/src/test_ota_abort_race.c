/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the cc3501e-bridge TI backend's OTA abort-vs-FINISH state
 * machine (issue #1123): cc3501e_hw_ota_abort() racing the deferred FINISH
 * work in cc3501e_hw_ota_pump() must never let a cancelled session install
 * or reboot into the image it was told to cancel.
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
 * false" but "the vendor finish/install sequence never (or did) run" --
 * the distinction issue #1123 cares about: a WRITE-phase abort must stop
 * FINISH before psa_fwu_finish()/install() ever touch flash; an abort
 * racing the install() call itself lands after the flash commit (which
 * psa_fwu cannot unwind) but must still gate the STAGED/reboot_pending
 * publish. */
static unsigned g_start_calls;
static unsigned g_write_calls;
static unsigned g_finish_calls;
static unsigned g_install_calls;
/* When true, the mock cc3501e_ota_psa_install() calls cc3501e_hw_ota_abort()
 * from INSIDE itself, right before returning success -- simulating the SPI
 * dispatch context's ABORT landing in the tightest possible window: after
 * the vendor flash commit, before ota_finish_step() publishes STAGED /
 * arms the reboot latch. */
static bool g_install_reentrant_abort;

static void mock_reset(void)
{
	g_start_calls             = 0u;
	g_write_calls             = 0u;
	g_finish_calls            = 0u;
	g_install_calls           = 0u;
	g_install_reentrant_abort = false;
	ota_reboot_pending        = false;
	ota_reboot_rc             = 0;
	reply_drained             = true;
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
	return true;
}

bool cc3501e_ota_psa_reject(void)
{
	return true;
}

bool cc3501e_ota_psa_clean(uint8_t slot)
{
	(void)slot;
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
 * phase needs exactly ONE psa_fwu_write block -- the whole FINISH sequence
 * then takes exactly 4 cc3501e_hw_ota_pump() ticks:
 *   1. START   -- cc3501e_ota_psa_start(), advances to WRITE
 *   2. WRITE   -- the one data block, advances finish_off to total_len
 *   3. (WRITE) -- finish_off >= total_len, advances to INSTALL
 *   4. INSTALL -- cc3501e_ota_psa_finish() + _install(), publishes STAGED
 *                 and arms ota_reboot_pending (unless abort intervened)
 * Chosen deliberately small + exact so the abort-injection tests below can
 * position an abort precisely between ticks 3 and 4. */
#define TEST_DATA_LEN     6u
#define TEST_MANIFEST_LEN 4u /* == cc3501e_ota_psa_manifest_size() above */
#define TEST_TOTAL_LEN    (TEST_MANIFEST_LEN + TEST_DATA_LEN)
#define TEST_FINISH_TICKS 4u

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
	cc3501e_hw_ota_pump(); /* ota_do_begin() is not chunked -- one tick completes it */
	zassert_equal(ota_state_now(), ALP_CC3501E_OTA_STATE_WRITING, "BEGIN must land in WRITING");
	zassert_equal(cc3501e_hw_ota_write(0u, data, sizeof data), CC3501E_HW_OK);
}

ZTEST_SUITE(cc3501e_ota_abort_race, NULL, NULL, NULL, NULL, NULL);

/* Baseline / control: no abort at all -- FINISH must still complete, and
 * must take MORE THAN ONE tick to do it (proves chunking actually chunks,
 * not just a single call in disguise). */
ZTEST(cc3501e_ota_abort_race, test_finish_completes_without_abort)
{
	reset_all();
	begin_and_write_all();

	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY, "FINISH queues, not immediate");

	cc3501e_hw_ota_pump(); /* tick 1: START */
	zassert_equal(g_start_calls, 1u);
	zassert_equal(g_install_calls, 0u, "must not reach INSTALL after only one tick -- chunked");
	zassert_equal(ota_reboot_pending, false);

	for (unsigned i = 0u; i < TEST_FINISH_TICKS - 1u; i++) {
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

/* THE #1123 REGRESSION -- abort lands after the WRITE phase has finished
 * streaming but BEFORE psa_fwu_finish()/install() ever run.  Must block
 * the flash-commit calls entirely, not just the state publish. */
ZTEST(cc3501e_ota_abort_race, test_abort_before_install_blocks_finish_and_install)
{
	reset_all();
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);

	/* Ticks 1-3: START, WRITE (the one data block), WRITE-exit -> INSTALL.
	 * psa_fwu_finish()/install() have NOT run yet at this point. */
	for (unsigned i = 0u; i < TEST_FINISH_TICKS - 1u; i++) {
		cc3501e_hw_ota_pump();
	}
	zassert_equal(g_finish_calls, 0u, "must not have reached INSTALL yet");
	zassert_equal(g_install_calls, 0u);

	zassert_equal(cc3501e_hw_ota_abort(), CC3501E_HW_OK, "abort while FINISH is mid-flight");

	/* Let the pump settle (bounded, generous margin over the 1 tick actually
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
 * -- ota.state and ota_reboot_pending -- must still never happen. */
ZTEST(cc3501e_ota_abort_race, test_abort_racing_install_still_blocks_stage_and_reboot)
{
	reset_all();
	begin_and_write_all();
	zassert_equal(cc3501e_hw_ota_finish(), CC3501E_HW_BUSY);

	g_install_reentrant_abort = true;
	for (unsigned i = 0u; i < TEST_FINISH_TICKS; i++) {
		cc3501e_hw_ota_pump();
	}

	zassert_equal(g_install_calls, 1u, "the flash commit itself is NOT retroactively undoable");
	zassert_equal(
	    ota_reboot_pending, false, "a cancelled session must never arm the swap-reboot (#1123)");
	zassert_equal(ota_state_now(),
	              ALP_CC3501E_OTA_STATE_IDLE,
	              "a cancelled session must never read STAGED (#1123)");
}
