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
	ring->seg     = seg;
	ring->size    = size;
	ring->enqueue = 0u;
	ring->cycle   = 1;
	/* Last TRB is a Link TRB pointing back to seg[0] (spec §4.11.5.1). Its
	 * cycle bit is set to the producer cycle as the ring crosses it.
	 * Ring Segment Base is a 64-bit address (spec §6.4.4.1). */
	seg[size - 1u].param_lo = (uint32_t)(uintptr_t)seg;
	seg[size - 1u].param_hi = (uint32_t)((uintptr_t)seg >> 32);
	seg[size - 1u].control  = XHCI_TRB_TYPE(XHCI_TRB_TYPE_LINK) | XHCI_TRB_LINK_TC;
}

void xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *in)
{
	struct xhci_trb *slot = &ring->seg[ring->enqueue];

	slot->param_lo = in->param_lo;
	slot->param_hi = in->param_hi;
	slot->status   = in->status;
	/* Producer owns the slot by setting its cycle bit to the producer cycle. */
	slot->control = (in->control & ~XHCI_TRB_CYCLE) | (ring->cycle ? XHCI_TRB_CYCLE : 0u);

	ring->enqueue++;
	if (ring->enqueue == ring->size - 1u) {
		/* Reached the Link TRB: stamp its cycle with the producer cycle,
		 * then toggle and wrap (spec §4.11.5.1). */
		struct xhci_trb *link = &ring->seg[ring->size - 1u];

		link->control = (link->control & ~XHCI_TRB_CYCLE) | (ring->cycle ? XHCI_TRB_CYCLE : 0u);
		ring->cycle ^= 1;
		ring->enqueue = 0u;
	}
}

void xhci_dcbaa_set(uint64_t *dcbaa, uint32_t slot, uint64_t ctx_phys)
{
	dcbaa[slot] = ctx_phys;
}

void xhci_build_slot_context(uint32_t *ctx,
                             uint32_t  route_string,
                             uint32_t  speed,
                             uint32_t  ctx_entries)
{
	/* §6.2.2 dword0: RouteString[19:0], Speed[23:20], ContextEntries[31:27]. */
	ctx[0] = (route_string & 0xFFFFFu) | ((speed & 0xFu) << 20) | ((ctx_entries & 0x1Fu) << 27);
}

void xhci_build_ep_context(
    uint32_t *ctx, uint32_t ep_type, uint32_t max_packet, uint64_t tr_dequeue_phys, int dcs)
{
	/* §6.2.3 dword1: EPType[5:3], MaxPacketSize[31:16]. */
	ctx[1] = ((ep_type & 0x7u) << 3) | ((max_packet & 0xFFFFu) << 16);
	/* dword2/3: TR Dequeue Pointer (16-byte aligned) | DCS[bit0]. */
	ctx[2] = ((uint32_t)(tr_dequeue_phys & 0xFFFFFFF0u)) | (dcs ? 1u : 0u);
	ctx[3] = (uint32_t)(tr_dequeue_phys >> 32);
}

void xhci_init_sequence(struct xhci_op_regs *op,
                        uint64_t             dcbaa_phys,
                        uint64_t             cmd_ring_phys,
                        uint32_t             max_slots)
{
	/* spec §4.2 init: program MaxSlotsEn, DCBAAP, CRCR (RCS=1), then run. */
	op->config    = (op->config & ~0xFFu) | (max_slots & 0xFFu);
	op->dcbaap_lo = (uint32_t)(dcbaa_phys & 0xFFFFFFC0u); /* 64-byte aligned */
	op->dcbaap_hi = (uint32_t)(dcbaa_phys >> 32);
	op->crcr_lo   = (uint32_t)(cmd_ring_phys & 0xFFFFFFC0u) | XHCI_CRCR_RCS;
	op->crcr_hi   = (uint32_t)(cmd_ring_phys >> 32);
	op->usbcmd |= XHCI_USBCMD_RS;
}
