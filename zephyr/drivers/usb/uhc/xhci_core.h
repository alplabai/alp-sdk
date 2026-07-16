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
#define XHCI_TRB_TYPE_LINK   6u  /* spec Table 6-91 */
#define XHCI_TRB_TYPE_NORMAL   1u  /* Normal (bulk/int data, §6.4.1.1) */
#define XHCI_TRB_TYPE_SETUP    2u  /* Setup Stage (§6.4.1.2.1) */
#define XHCI_TRB_TYPE_DATA     3u  /* Data Stage (§6.4.1.2.2) */
#define XHCI_TRB_TYPE_STATUS   4u  /* Status Stage (§6.4.1.2.3) */
#define XHCI_TRB_TYPE_ENABLE_SLOT 9u  /* Enable Slot Command (§6.4.3.2) */
#define XHCI_TRB_TYPE_ADDRESS_DEVICE 11u /* Address Device Command (§6.4.3.4) */
#define XHCI_TRB_TYPE_NOOP_CMD 23u /* No Op Command (§6.4.3.10) */
#define XHCI_TRB_TYPE_TRANSFER_EVENT 32u /* Transfer Event (§6.4.2.1) */
#define XHCI_TRB_TYPE_CMD_COMPLETION 33u /* Command Completion Event (§6.4.2.2) */
#define XHCI_TRB_TYPE_PORT_STATUS 34u /* Port Status Change Event (§6.4.2.3) */

/* Control-transfer TRB control-word bits (§6.4.1.2). */
#define XHCI_TRB_IDT          (1u << 6)  /* Immediate Data (Setup) */
#define XHCI_TRB_IOC          (1u << 5)  /* Interrupt On Completion */
#define XHCI_TRB_DIR_IN       (1u << 16) /* Data/Status stage direction = IN */
#define XHCI_TRB_TRT_IN       (3u << 16) /* Setup TRT = IN data stage */
#define XHCI_SLOT_ID(c)       ((uint32_t)(c) << 24) /* command TRB Slot ID field */
/* Event/completion helpers: completion code is status[31:24]; SUCCESS = 1.
 * A Command Completion Event carries the Slot ID in control[31:24]. */
#define XHCI_TRB_GET_CC(status)   (((status) >> 24) & 0xFFu)
#define XHCI_TRB_GET_SLOT(control) (((control) >> 24) & 0xFFu)
#define XHCI_CC_SUCCESS      1u

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

/* CPU-local TCM -> system-bus global-alias map (one per M55 core: HP and HE
 * each have their OWN ITCM/DTCM global alias window -- see xhci_local_to_global()
 * below). Callers fill this from the active core's DT (itcm/dtcm `reg` +
 * `global_base`), never from a hardcoded core's aliases. */
struct xhci_tcm_map {
	uintptr_t itcm_base;
	uintptr_t itcm_size;
	uint64_t  itcm_global_base;
	uintptr_t dtcm_base;
	uintptr_t dtcm_size;
	uint64_t  dtcm_global_base;
};

/* Translate a CPU-local pointer (ITCM/DTCM) into the global bus alias the xHCI
 * DMA master must be handed, per `map`. Addresses outside both TCM windows
 * (e.g. already-global SRAM) pass through unchanged. Pure/arch-neutral so it is
 * host-unit-testable against ANY core's map -- there is no "the" TCM alias. */
uint64_t xhci_local_to_global(const struct xhci_tcm_map *map, const void *p);

#endif /* ALP_XHCI_CORE_H */
