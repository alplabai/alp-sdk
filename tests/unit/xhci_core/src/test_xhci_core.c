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
