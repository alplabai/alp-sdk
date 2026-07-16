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

ZTEST(alp_xhci_core, test_init_sequence_writes_expected_regs)
{
	struct xhci_op_regs op;
	memset(&op, 0, sizeof(op));

	/* Use non-trivial addresses to pin both the 64-byte alignment masks
	 * and the high-word split.  Both values are already 64-byte aligned
	 * (low 6 bits zero) so the masks do not alter them -- confirming the
	 * function passes them through unchanged rather than masking them away. */
	xhci_init_sequence(
	    &op, 0x1234567800002040ull /*dcbaa*/, 0x00000000DEAD2080ull /*cmd ring*/, 8u);

	zassert_equal(op.config & 0xFFu, 8u, "CONFIG.MaxSlotsEn = 8");
	zassert_equal(op.dcbaap_lo, 0x00002040u, "DCBAAP low");
	zassert_equal(op.dcbaap_hi, 0x12345678u, "DCBAAP high");
	/* CRCR low = cmd_ring_phys (64-byte aligned) | RCS(bit0)=1. */
	zassert_equal(op.crcr_lo, 0xDEAD2080u | 1u, "CRCR low with RCS=1");
	zassert_equal(op.crcr_hi, 0u, "CRCR high");
	zassert_true((op.usbcmd & 1u) != 0, "USBCMD.R/S set");
}

/* issue #752: xHCI DMA must translate a CPU-local TCM pointer through the
 * ACTIVE core's own global alias window, never a hardcoded one.  M55-HP and
 * M55-HE have DIFFERENT alias windows for the identical local offset (Alif
 * DFP / zephyr_alif fork ensemble_rtss_hp.dtsi / ensemble_rtss_he.dtsi, also
 * transcribed into the board DTS `&itcm`/`&dtcm` `global_base` overrides):
 *   M55-HP: ITCM 0x00000000/256KiB -> global 0x50000000; DTCM 0x20000000/1MiB -> 0x50800000
 *   M55-HE: ITCM 0x00000000/256KiB -> global 0x58000000; DTCM 0x20000000/256KiB -> 0x58800000
 * These two maps are the regression case: reusing the HE map (or any single
 * hardcoded map) for an HP pointer would silently mistranslate it. */
static const struct xhci_tcm_map hp_map = {
	.itcm_base        = 0x00000000u,
	.itcm_size        = 256u * 1024u,
	.itcm_global_base = 0x50000000u,
	.dtcm_base        = 0x20000000u,
	.dtcm_size        = 1024u * 1024u,
	.dtcm_global_base = 0x50800000u,
};

static const struct xhci_tcm_map he_map = {
	.itcm_base        = 0x00000000u,
	.itcm_size        = 256u * 1024u,
	.itcm_global_base = 0x58000000u,
	.dtcm_base        = 0x20000000u,
	.dtcm_size        = 256u * 1024u,
	.dtcm_global_base = 0x58800000u,
};

ZTEST(alp_xhci_core, test_local_to_global_itcm_uses_the_active_core_map)
{
	/* Same local ITCM offset (0x40), two different core maps -> two
	 * different global aliases. Neither may equal the other core's alias. */
	uint64_t hp_g = xhci_local_to_global(&hp_map, (const void *)(uintptr_t)0x00000040u);
	uint64_t he_g = xhci_local_to_global(&he_map, (const void *)(uintptr_t)0x00000040u);

	zassert_equal(hp_g, 0x50000040ull, "HP ITCM+0x40 -> HP global alias");
	zassert_equal(he_g, 0x58000040ull, "HE ITCM+0x40 -> HE global alias (NOT the HP one)");
	zassert_true(hp_g != he_g, "HP and HE aliases must never collide");
}

ZTEST(alp_xhci_core, test_local_to_global_dtcm_uses_the_active_core_map)
{
	uint64_t hp_g = xhci_local_to_global(&hp_map, (const void *)(uintptr_t)0x20001000u);
	uint64_t he_g = xhci_local_to_global(&he_map, (const void *)(uintptr_t)0x20001000u);

	zassert_equal(hp_g, 0x50801000ull, "HP DTCM+0x1000 -> HP global alias");
	zassert_equal(he_g, 0x58801000ull, "HE DTCM+0x1000 -> HE global alias (NOT the HP one)");
}

ZTEST(alp_xhci_core, test_local_to_global_dtcm_upper_boundary)
{
	/* Last valid HE DTCM byte (base+size-1) still translates ... */
	uintptr_t last_valid = 0x20000000u + (256u * 1024u) - 1u;
	uint64_t  g_in       = xhci_local_to_global(&he_map, (const void *)last_valid);

	zassert_equal(g_in, 0x58800000ull + (256u * 1024u) - 1u, "last in-range byte translates");

	/* ... the first byte past the HE DTCM window (which IS still inside the
	 * larger HP DTCM window) must NOT alias into DTCM under the HE map --
	 * it is outside HE's window and passes through unchanged (not global
	 * SRAM/TCM on this core). */
	uintptr_t just_past = 0x20000000u + (256u * 1024u);
	uint64_t  g_out     = xhci_local_to_global(&he_map, (const void *)just_past);

	zassert_equal(g_out, (uint64_t)just_past, "one past DTCM end passes through unchanged");
}

ZTEST(alp_xhci_core, test_local_to_global_passthrough_for_non_tcm_address)
{
	/* An address already outside both TCM windows (e.g. SRAM0 0x02000000) is
	 * already a global/system-bus address -- pass through unchanged. */
	uint64_t g = xhci_local_to_global(&hp_map, (const void *)(uintptr_t)0x02000000u);

	zassert_equal(g, 0x02000000ull, "non-TCM address passes through unchanged");
}

ZTEST(alp_xhci_core, test_local_to_global_null_pointer_rejected)
{
	/* itcm_base is 0 on every known map, so NULL falls inside the ITCM
	 * window unless explicitly rejected -- it must NOT silently become a
	 * plausible-looking global ITCM address. */
	zassert_equal(
	    xhci_local_to_global(&hp_map, NULL), 0ull, "NULL never aliases into ITCM, on HP...");
	zassert_equal(xhci_local_to_global(&he_map, NULL), 0ull, "...or HE");
}
