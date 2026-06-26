/*
 * SPDX-License-Identifier: Apache-2.0
 * Host unit tests for xhci_core (rings/contexts/init) -- native_sim, no controller.
 */
#include <string.h>
#include <zephyr/ztest.h>
#include "xhci_core.h"

ZTEST_SUITE(alp_xhci_core, NULL, NULL, NULL, NULL, NULL);

/* A ring of N TRBs: N-1 usable + 1 Link TRB. Enqueuing N-1 entries fills it to
 * the Link TRB; the next enqueue wraps to index 0 and toggles the producer cycle. */
ZTEST(alp_xhci_core, test_ring_enqueue_cycle_and_link_wrap)
{
	struct xhci_trb  seg[4];
	struct xhci_ring ring;

	xhci_ring_init(&ring, seg, 4);
	zassert_equal(ring.cycle, 1, "initial producer cycle is 1");
	zassert_equal(ring.enqueue, 0u, "starts at index 0");

	struct xhci_trb in = { .param_lo = 0xAA, .control = 0 };

	/* 3 usable slots (indices 0,1,2); index 3 is the Link TRB. */
	xhci_ring_enqueue(&ring, &in);
	zassert_true((seg[0].control & XHCI_TRB_CYCLE) != 0, "TRB0 carries producer cycle=1");
	xhci_ring_enqueue(&ring, &in);
	zassert_true((seg[1].control & XHCI_TRB_CYCLE) != 0, "TRB1 carries producer cycle=1");
	xhci_ring_enqueue(&ring, &in); /* this fills to the Link TRB -> wrap */
	zassert_true((seg[2].control & XHCI_TRB_CYCLE) != 0, "TRB2 carries producer cycle=1");

	zassert_equal(ring.enqueue, 0u, "wrapped back to index 0");
	zassert_equal(ring.cycle, 0, "producer cycle toggled after the Link TRB");
	/* The Link TRB (seg[3]) must be a Link type with TC set, cycle = pre-toggle (1). */
	zassert_equal(XHCI_TRB_GET_TYPE(seg[3].control), XHCI_TRB_TYPE_LINK, "seg[3] is a Link TRB");
	zassert_true((seg[3].control & XHCI_TRB_LINK_TC) != 0, "Link TRB has Toggle-Cycle set");
	zassert_true((seg[3].control & XHCI_TRB_CYCLE) != 0,
	             "Link TRB cycle was the producer cycle (1)");
}

ZTEST(alp_xhci_core, test_dcbaa_and_context_build)
{
	uint64_t dcbaa[8] = { 0 };
	xhci_dcbaa_set(dcbaa, 1, 0x12340000ull);
	zassert_equal(dcbaa[1], 0x12340000ull, "DCBAA slot 1 holds the context phys");

	/* Slot context dword0 (spec §6.2.2): route(19:0) | speed(23:20) | ctx_entries(31:27). */
	uint32_t sc[8] = { 0 };
	xhci_build_slot_context(sc, 0x5u /*route*/, 3u /*HS*/, 1u /*entries*/);
	zassert_equal(sc[0] & 0xFFFFFu, 0x5u, "route string");
	zassert_equal((sc[0] >> 20) & 0xFu, 3u, "speed");
	zassert_equal((sc[0] >> 27) & 0x1Fu, 1u, "context entries");

	/* EP context (spec §6.2.3): dword1 ep_type(5:3) | max_packet(31:16);
	 * dword2/3 = TR dequeue ptr | DCS(bit0). */
	uint32_t ep[8] = { 0 };
	xhci_build_ep_context(
	    ep, 4u /* Control Bidirectional (xHCI spec §6.2.3) */, 64u, 0xCAFE0000ull, 1);
	zassert_equal((ep[1] >> 3) & 0x7u, 4u, "ep type");
	zassert_equal((ep[1] >> 16) & 0xFFFFu, 64u, "max packet size");
	zassert_equal(ep[2] & 0x1u, 1u, "dequeue cycle state (DCS)");
	zassert_equal(ep[2] & ~0xFu, (uint32_t)(0xCAFE0000ull & ~0xFull), "TR dequeue ptr lo");
}
