/* Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure-C, arch-neutral xHCI host logic (rings/contexts/init sequence) per the
 * public xHCI specification.  No MMIO/Zephyr/Cortex deps -- host-unit-testable.
 */
#ifndef ALP_XHCI_CORE_H
#define ALP_XHCI_CORE_H
#include <stdint.h>
#include <stddef.h>

/* xHCI Transfer Request Block (spec §6.4): 4 x 32-bit. */
struct xhci_trb {
	uint32_t param_lo;
	uint32_t param_hi;
	uint32_t status;
	uint32_t control;
};

/* TRB control-word fields (spec §6.4.4). */
#define XHCI_TRB_CYCLE       (1u << 0)
#define XHCI_TRB_LINK_TC     (1u << 1) /* Link TRB Toggle Cycle */
#define XHCI_TRB_TYPE(t)     ((uint32_t)(t) << 10)
#define XHCI_TRB_GET_TYPE(c) (((c) >> 10) & 0x3Fu)
#define XHCI_TRB_TYPE_LINK   6u /* spec Table 6-91 */

/* A producer ring: `size` TRBs, the last reserved as a Link TRB. */
struct xhci_ring {
	struct xhci_trb *seg;
	uint32_t         size;    /* total TRBs incl. the Link TRB */
	uint32_t         enqueue; /* producer index */
	int              cycle;   /* producer cycle state (0/1) */
};

void xhci_ring_init(struct xhci_ring *ring, struct xhci_trb *seg, uint32_t size);
void xhci_ring_enqueue(struct xhci_ring *ring, const struct xhci_trb *in);

#endif /* ALP_XHCI_CORE_H */
