/* Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0 */
#include <string.h>
#include "xhci_core.h"

void xhci_ring_init(struct xhci_ring *ring, struct xhci_trb *seg, uint32_t size)
{
	/* A ring needs at least 1 usable TRB + 1 Link TRB. */
	if (size < 2u) {
		return;
	}
	memset(seg, 0, (size_t)size * sizeof(*seg));
	ring->seg = seg;
	ring->size = size;
	ring->enqueue = 0u;
	ring->cycle = 1;
	/* Last TRB is a Link TRB pointing back to seg[0] (spec §4.11.5.1). Its
	 * cycle bit is set to the producer cycle as the ring crosses it.
	 * Ring Segment Base is a 64-bit address (spec §6.4.4.1). */
	seg[size - 1u].param_lo = (uint32_t)(uintptr_t)seg;
	seg[size - 1u].param_hi = (uint32_t)((uintptr_t)seg >> 32);
	seg[size - 1u].control = XHCI_TRB_TYPE(XHCI_TRB_TYPE_LINK) | XHCI_TRB_LINK_TC;
}

void xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *in)
{
	struct xhci_trb *slot = &ring->seg[ring->enqueue];

	slot->param_lo = in->param_lo;
	slot->param_hi = in->param_hi;
	slot->status = in->status;
	/* Producer owns the slot by setting its cycle bit to the producer cycle. */
	slot->control = (in->control & ~XHCI_TRB_CYCLE) | (ring->cycle ? XHCI_TRB_CYCLE : 0u);

	ring->enqueue++;
	if (ring->enqueue == ring->size - 1u) {
		/* Reached the Link TRB: stamp its cycle with the producer cycle,
		 * then toggle and wrap (spec §4.11.5.1). */
		struct xhci_trb *link = &ring->seg[ring->size - 1u];

		link->control = (link->control & ~XHCI_TRB_CYCLE) |
				(ring->cycle ? XHCI_TRB_CYCLE : 0u);
		ring->cycle ^= 1;
		ring->enqueue = 0u;
	}
}
