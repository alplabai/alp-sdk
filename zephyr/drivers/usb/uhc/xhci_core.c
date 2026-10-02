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
	seg[size - 1u].param_hi = (uint32_t)((uint64_t)(uintptr_t)seg >> 32);
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

uint32_t xhci_ring_next_index(const struct xhci_ring *ring, uint32_t index)
{
	uint32_t next = index + 1u;

	if (next == ring->size - 1u) {
		next = 0u;
	}
	return next;
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

void xhci_build_ep_context(uint32_t *ctx,
                           uint32_t  ep_type,
                           uint32_t  max_packet,
                           uint64_t  tr_dequeue_phys,
                           int       dcs,
                           uint32_t  interval,
                           uint32_t  avg_trb_len)
{
	/* §6.2.3 dword0: Interval[23:16]. */
	ctx[0] = (interval & 0xFFu) << 16;
	/* dword1: CErr[2:1] (set to the spec-recommended max of 3 retries
	 * before the xHC reports an error up), EPType[5:3], MaxPacketSize[31:16]. */
	ctx[1] = (3u << 1) | ((ep_type & 0x7u) << 3) | ((max_packet & 0xFFFFu) << 16);
	/* dword2/3: TR Dequeue Pointer (16-byte aligned) | DCS[bit0]. */
	ctx[2] = ((uint32_t)(tr_dequeue_phys & 0xFFFFFFF0u)) | (dcs ? 1u : 0u);
	ctx[3] = (uint32_t)(tr_dequeue_phys >> 32);
	/* dword4: Average TRB Length[15:0] (spec requires nonzero); Max ESIT
	 * Payload Lo[31:16] left 0 -- only meaningful for isochronous/SS
	 * endpoints, neither of which this USB 2.0 bulk/control/interrupt-only
	 * driver uses. */
	ctx[4] = avg_trb_len & 0xFFFFu;
}

int xhci_validate_xfer_len(size_t len, size_t buf_room)
{
	if (len == 0u || len > buf_room) {
		return -EINVAL;
	}
	if (len > XHCI_TRB_MAX_LEN) {
		return -EINVAL;
	}
	return 0;
}

uint8_t xhci_dci_for_ep(uint8_t ep_addr)
{
	uint8_t num    = ep_addr & 0x0Fu;
	uint8_t dir_in = (ep_addr & 0x80u) != 0u;

	if (num == 0u) {
		return 1u;
	}
	return (uint8_t)(2u * num + (dir_in ? 1u : 0u));
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

uint64_t xhci_local_to_global(const struct xhci_tcm_map *map, const void *p)
{
	uintptr_t a = (uintptr_t)p;

	/* A NULL DMA pointer must never be handed to the xHC as a "translated"
	 * address: on every known map itcm_base == 0, so without this guard
	 * NULL would silently alias into a plausible-looking global ITCM
	 * address (0 falls inside [itcm_base, itcm_base+itcm_size)) instead of
	 * being caught by the caller. Every real caller (uhc_xhci_alif.c's
	 * xhci_l2g()) always passes a live buffer address, so this only ever
	 * fires on a programming error. */
	if (p == NULL) {
		return 0;
	}

	/* DTCM and ITCM windows never overlap; check both explicitly rather than
	 * assuming a base of 0 (a hardcoded-base bug is exactly what this
	 * function replaces -- see uhc_xhci_alif.c's xhci_l2g()). */
	if (a >= map->dtcm_base && a < map->dtcm_base + map->dtcm_size) {
		return (uint64_t)(a - map->dtcm_base) + map->dtcm_global_base;
	}
	if (a >= map->itcm_base && a < map->itcm_base + map->itcm_size) {
		return (uint64_t)(a - map->itcm_base) + map->itcm_global_base;
	}
	/* Already outside both local TCM windows (e.g. SRAM, or already a global
	 * alias) -- the xHC's system-bus master reaches it unchanged. */
	return (uint64_t)a;
}
