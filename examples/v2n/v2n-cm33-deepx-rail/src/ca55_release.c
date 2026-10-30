/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * v2n_cm33_release_ca55() -- start CA55 core 0 from the CM33 (alp-sdk#2289).
 *
 * DECISION (issue #2289).  In CM33 cold boot only PD_AWO is powered and the
 * CA55 cluster stays in reset.  The CM33 releases core 0 itself, after the
 * DEEPX rail sequence, with the reset vector pointed at a CM33-staged TF-A
 * BL2 through SYS_ACPU_CFG_RVAL0/RVAH0.  BL2 keeps training DDR as today and
 * no OTP is involved.
 *
 * SOURCE OF THE REGISTER FACTS.  The RZ/V2N FSP / hal_renesas headers
 * (cpg_iodefine.h, sysc_iodefine.h) are NOT in this tree, so nothing below
 * can cite a header line.  Every offset and bit field is taken verbatim from
 * the alp-sdk#2289 comment that re-checked them against the RZ/V2N hardware
 * manual R01UH1071EJ0120 Rev.1.20: S4.3 Table 4.3-1 / S4.4 Table 4.4-4
 * (bases), S4.3.3.2.89-90 (RVAL0/RVAH0), S4.4.4.11 (CPG_RST_m), S4.4.4.22
 * (CPG_LP_CA55_CTL1), S4.4.4.23 (CPG_LP_CA55_CTL2), S2.2.2.2.1 Table 2.2-7
 * (cold-reset release, software control by the CM33).
 * TODO(alp-sdk#2289): cross-check every constant against the FSP headers
 * before the first bench run; nothing here has run on silicon.
 *
 * UNVERIFIED: the CM33 security state and whether CPG/SYS reset control is
 * writable from non-secure.  The _NS aliases below are an assumption (the
 * board file uses _NS MHU bases, which proves nothing about CPG/SYS), and
 * tests/scripts/test_provision_uboot.py:123 uses SYS at 0x10430000.
 * TODO(alp-sdk#2289): cite the manual table row for the 0x5042xxxx /
 * 0x5043xxxx aliases and the CPG access attribute.
 */

#include <zephyr/arch/cpu.h>

#include "ca55_release.h"

/* TODO(alp-sdk#2289): NS alias unverified (see file header). */
#define CPG_BASE_NS 0x50420000UL /* S4.4 Table 4.4-4 */
#define SYS_BASE_NS 0x50430000UL /* S4.3 Table 4.3-1 */

/* Offsets/bits below: S4.3.3.2.89-90 (RVAL0/RVAH0), S4.4.4.11 (CPG_RST_m),
 * S4.4.4.22 (CPG_LP_CA55_CTL1), S4.4.4.23 (CPG_LP_CA55_CTL2).  Taken from the
 * issue comment; TODO(alp-sdk#2289): confirm each bit row vs the manual/FSP. */

#define SYS_ACPU_CFG_RVAL0 (SYS_BASE_NS + 0x0624UL) /* [31:2] = RVBARADDR[31:2] */
#define SYS_ACPU_CFG_RVAH0 (SYS_BASE_NS + 0x0628UL) /* [7:0]  = RVBARADDR[39:32] */

/* CPG_RST_m = CPG + 0900h + m*4.  1b = reset off.  Bits [31:16] are per-bit
 * write enables (RSTBn_WEN): only bits whose enable is set change.  Table
 * 4.4-22 puts CA55_RESET0..13 in RST_0 and CA55_RESET14..16 in RST_1. */
#define CPG_RST(m)     (CPG_BASE_NS + 0x0900UL + (uint32_t)(m) * 4UL)
#define RST0_CA55_MASK 0x00003FFFUL /* RSTB[13:0] */
#define RST1_CA55_MASK 0x00000007UL /* RSTB[2:0]  */
#define RST_WEN(mask)  ((mask) << 16)

#define CPG_LP_CA55_CTL1 (CPG_BASE_NS + 0x0C20UL)
#define CTL1_CLUSTERPREQ (1UL << 0)
#define CTL1_PSTATE_POS  1u
#define CTL1_PSTATE_MASK (0x7FUL << CTL1_PSTATE_POS)
#define CTL1_PSTATE_ON   0x48UL /* CLUSTERPSTATE: 48h = ON */
#define CTL1_PDENY       (1UL << 8)
#define CTL1_PACCEPT     (1UL << 9)

#define CPG_LP_CA55_CTL2 (CPG_BASE_NS + 0x0C24UL)
#define CTL2_COREPREQ0   (1UL << 0)
#define CTL2_PSTATE_POS  1u
#define CTL2_PSTATE_MASK (0x3FUL << CTL2_PSTATE_POS)
#define CTL2_PSTATE_ON   0x08UL /* COREPSTATE0: 08h = ON */
#define CTL2_PDENY       (1UL << 7)
#define CTL2_PACCEPT     (1UL << 8)

#define POLL_STEP_US 100u
#define POLL_MAX     1000u /* 100 ms per poll */

static uint32_t rd(uintptr_t addr)
{
	return sys_read32(addr);
}

static void wr(uintptr_t addr, uint32_t v)
{
	sys_write32(v, addr);
}

/* Read-modify-write: the manual's `*` bits must be preserved. */
static void rmw(uintptr_t addr, uint32_t clear, uint32_t set)
{
	wr(addr, (rd(addr) & ~clear) | set);
}

/* Bounded poll until (reg & mask) == want.  Never spins forever. */
static alp_status_t poll(uintptr_t addr, uint32_t mask, uint32_t want)
{
	for (uint32_t i = 0; i < POLL_MAX; i++) {
		if ((rd(addr) & mask) == want) return ALP_OK;
		alp_delay_us(POLL_STEP_US);
	}
	return ALP_ERR_TIMEOUT;
}

/*
 * TODO(alp-sdk#2289): AWO -> ALL_ON entry, HW manual S4.5.3.1.1 Table 4.5-4.
 * PD_OTHERS / PD_CA55 / PD_DDR0 are NOT powered in CM33 cold boot, so the
 * release below hangs or faults without it.  The issue lists the steps
 * (CPG_RST_n assert, CPG_OTHERS_INI.OTHERS_RST, CPG_LP_PWC_CTL1.OTHERS_ON_TRG,
 * CPG_LP_PWC_CTL2.PWEN poll, CPG_BUS_12_MSTOP, CPG_*_STBY / CPG_PLL*_MON,
 * CPG_LP_PMU_CTL1, OTP_HANDSHAKE_MON.DFT_DONE) but gives NO offsets for those
 * registers, and they are not in the tree.  Not guessed: this returns
 * ALP_ERR_NOSUPPORT so v2n_cm33_release_ca55() fails closed with the CA55
 * held until the offsets are taken from the manual / FSP headers.
 */
static alp_status_t all_on_entry(void)
{
	return ALP_ERR_NOSUPPORT;
}

alp_status_t v2n_cm33_release_ca55(uint64_t entry_addr)
{
	/* RVAL0[1:0] read 0 (4-byte aligned); RVAH0 carries only bits 39:32. */
	if (entry_addr == 0u || (entry_addr & 0x3u) != 0u || (entry_addr >> 40) != 0u)
		return ALP_ERR_INVAL;

	/* Table 4.5-4: PD_OTHERS / PD_CA55 / PD_DDR0 on, before anything below. */
	alp_status_t s = all_on_entry();
	if (s != ALP_OK) return s;

	/* The vector is sampled when reset is applied, so stage it first.  Do
	 * not rely on the OTP default: write RVAL0/RVAH0 explicitly (the manual
	 * does not say which wins when they are left unwritten; bench step 5). */
	wr(SYS_ACPU_CFG_RVAL0, (uint32_t)(entry_addr & 0xFFFFFFFCu));
	wr(SYS_ACPU_CFG_RVAH0, (uint32_t)(entry_addr >> 32));

	/* Table 2.2-7 (CM33 software control), read-modify-write throughout.
	 * Cluster, then core 0: state = ON and request. */
	rmw(CPG_LP_CA55_CTL1, CTL1_PSTATE_MASK, (CTL1_PSTATE_ON << CTL1_PSTATE_POS) | CTL1_CLUSTERPREQ);
	rmw(CPG_LP_CA55_CTL2, CTL2_PSTATE_MASK, (CTL2_PSTATE_ON << CTL2_PSTATE_POS) | CTL2_COREPREQ0);

	/* Reset release: RST_1 first, then RST_0, RSTB = 1 with their WEN bits. */
	wr(CPG_RST(1), RST_WEN(RST1_CA55_MASK) | RST1_CA55_MASK);
	/* TODO(alp-sdk#2289): the table polls CPG_RSTMON_0 (CPG+0A00h) here and
	 * after the RST_0 write, "= 0 for the CA55 bits".  The issue gives the
	 * offset but not the CA55 bit mask, so the polls are omitted rather than
	 * guessed; the ACCEPT polls below are the only completion check. */
	wr(CPG_RST(0), RST_WEN(RST0_CA55_MASK) | RST0_CA55_MASK);

	/* From here the CA55 is running, so a failure can no longer re-hold it;
	 * drop the requests on the way out and report. */
	s = poll(CPG_LP_CA55_CTL1, CTL1_PACCEPT | CTL1_PDENY, CTL1_PACCEPT);
	if (s == ALP_OK) s = poll(CPG_LP_CA55_CTL2, CTL2_PACCEPT | CTL2_PDENY, CTL2_PACCEPT);
	if (s != ALP_OK && ((rd(CPG_LP_CA55_CTL1) & CTL1_PDENY) || (rd(CPG_LP_CA55_CTL2) & CTL2_PDENY)))
		s = ALP_ERR_IO;

	rmw(CPG_LP_CA55_CTL1, CTL1_CLUSTERPREQ, 0u);
	rmw(CPG_LP_CA55_CTL2, CTL2_COREPREQ0, 0u);
	if (s == ALP_OK) s = poll(CPG_LP_CA55_CTL1, CTL1_PACCEPT, 0u);
	if (s == ALP_OK) s = poll(CPG_LP_CA55_CTL2, CTL2_PACCEPT, 0u);
	return s;
}
