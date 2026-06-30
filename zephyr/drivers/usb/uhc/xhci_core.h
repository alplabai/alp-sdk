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

/* DCBAA helpers (spec §6.1). */
void xhci_dcbaa_set(uint64_t *dcbaa, uint32_t slot, uint64_t ctx_phys);

/* Context builders (spec §6.2.2 slot, §6.2.3 endpoint). */
void xhci_build_slot_context(uint32_t *ctx,
                             uint32_t  route_string,
                             uint32_t  speed,
                             uint32_t  ctx_entries);
void xhci_build_ep_context(
    uint32_t *ctx, uint32_t ep_type, uint32_t max_packet, uint64_t tr_dequeue_phys, int dcs);

/* xHCI operational registers (spec §5.4), offsets from the op base
 * (= CAP base + CAPLENGTH).  Split 64-bit regs into lo/hi for portability. */
struct xhci_op_regs {
	uint32_t usbcmd;   /* 0x00 */
	uint32_t usbsts;   /* 0x04 */
	uint32_t pagesize; /* 0x08 */
	uint32_t rsvd0[2];
	uint32_t dnctrl;  /* 0x14 */
	uint32_t crcr_lo; /* 0x18 */
	uint32_t crcr_hi; /* 0x1C */
	uint32_t rsvd1[4];
	uint32_t dcbaap_lo; /* 0x30 */
	uint32_t dcbaap_hi; /* 0x34 */
	uint32_t config;    /* 0x38 */
};
#define XHCI_USBCMD_RS (1u << 0)
#define XHCI_CRCR_RCS  (1u << 0)

void xhci_init_sequence(struct xhci_op_regs *op,
                        uint64_t             dcbaa_phys,
                        uint64_t             cmd_ring_phys,
                        uint32_t             max_slots);

#endif /* ALP_XHCI_CORE_H */
