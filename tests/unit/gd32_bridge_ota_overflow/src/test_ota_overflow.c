/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression tests for the OTA_WRITE_CHUNK offset/length wrap guard (#470,
 * tracked by #563): `off + dlen` is wire-controlled and used to overflow
 * uint32_t before the old `> OTA_SLOT_SIZE` comparison ran, so a
 * near-UINT32_MAX offset wrapped into an apparently in-range chunk and was
 * about to be programmed at `ota_slot_base(slot) + off` -- itself wrapped,
 * landing far outside the slot.
 *
 * src/ota.c is source-included below (see CMakeLists.txt for why): it gives
 * the test direct access to `h_write()` and its file-scope OTA session
 * state without exercising OTA_BEGIN's A/B metadata read, which dereferences
 * real flash-mapped addresses (OTA_META_REC0/1) that don't exist in a host
 * process.
 */

#define BRIDGE_OTA_PARTITIONED 1

#include <string.h>
#include <zephyr/ztest.h>

#include "ota.c" /* NOLINT -- intentional source inclusion, see CMakeLists.txt */

/* Puts the OTA session into the state h_write() expects right after a
 * successful OTA_BEGIN, without touching the (host-unreachable) metadata
 * flash region. */
static void ota_session_open(void)
{
	s_state        = OTA_ST_READY;
	s_inactive     = OTA_SLOT_A;
	s_last_off     = 0u;
	s_img_len      = OTA_SLOT_SIZE;
	s_expected_crc = 0u;
	s_err          = 0u;
}

ZTEST(gd32_bridge_ota_overflow, test_write_chunk_rejects_near_uint32_max_offset)
{
	ota_session_open();

	/* off = 0xFFFFFFF0, dlen = 32: off + dlen wraps to 0x10 in uint32_t
	 * arithmetic -- comfortably < OTA_SLOT_SIZE under the old additive
	 * check, which would have accepted the chunk and gone on to compute
	 * ota_slot_base(slot) + off (itself wrapping) as the program address. */
	uint8_t req[5 + 32];
	wr_u32(&req[0], 0xFFFFFFF0u);
	req[4] = 32u;
	memset(&req[5], 0xAAu, 32u);

	uint8_t                    reply[4] = { 0 };
	size_t                     rlen     = 0u;
	const gd32_bridge_status_t st       = h_write(req, sizeof req, reply, sizeof reply, &rlen);

	zassert_equal(st, STATUS_OUT_OF_RANGE, "wrapped offset must be rejected, not wrapped-accepted");
	zassert_equal(s_state, OTA_ST_ERROR, "guard must fault the session, matching in-range rejects");
	zassert_equal(rlen, 0u, "no reply staged for a rejected chunk");
}

ZTEST(gd32_bridge_ota_overflow, test_write_chunk_rejects_offset_at_uint32_max)
{
	ota_session_open();

	/* off = UINT32_MAX, dlen = 1: off + dlen wraps to exactly 0, which
	 * passed even a naive `<= OTA_SLOT_SIZE` guard outright under the
	 * pre-fix arithmetic. */
	uint8_t req[6];
	wr_u32(&req[0], 0xFFFFFFFFu);
	req[4] = 1u;
	req[5] = 0x55u;

	uint8_t                    reply[4] = { 0 };
	size_t                     rlen     = 0u;
	const gd32_bridge_status_t st       = h_write(req, sizeof req, reply, sizeof reply, &rlen);

	zassert_equal(
	    st, STATUS_OUT_OF_RANGE, "offset at UINT32_MAX must be rejected, not wrapped-accepted");
	zassert_equal(s_state, OTA_ST_ERROR, "guard must fault the session, matching in-range rejects");
}

ZTEST(gd32_bridge_ota_overflow, test_write_chunk_accepts_in_range_offset)
{
	ota_session_open();

	/* Sanity check the guard doesn't reject legitimate small offsets:
	 * off=0, dlen=1 is trivially in range.  ota_fmc_program() falls
	 * through to the weak no-op default (returns false, no real FMC
	 * backend linked here), so the expected outcome is STATUS_IO from
	 * the *program* step, not STATUS_OUT_OF_RANGE from the guard --
	 * proving the bound check itself let the request through. */
	uint8_t req[6] = { 0u, 0u, 0u, 0u, 1u, 0x55u };

	uint8_t                    reply[4] = { 0 };
	size_t                     rlen     = 0u;
	const gd32_bridge_status_t st       = h_write(req, sizeof req, reply, sizeof reply, &rlen);

	zassert_equal(st, STATUS_IO, "in-range offset must pass the bound guard");
}

ZTEST_SUITE(gd32_bridge_ota_overflow, NULL, NULL, NULL, NULL, NULL);
