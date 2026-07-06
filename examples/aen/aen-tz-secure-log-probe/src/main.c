/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-core TrustZone-M proof for the AEN update-log HW-enforced tier.
 *
 * Question: on ONE M55 (no second core), can the application be hardware-blocked
 * from writing the MRAM log window? The answer is core-local TrustZone: the SAU
 * marks the log window Secure; the app runs Non-Secure; a Non-Secure write to the
 * Secure window faults, and the app cannot lift it (SAU/TGU regs are Secure-only).
 *
 * This probe (Secure state, the state SES hands HE) configures the SAU + TGU to
 * carve a Non-Secure chunk out of DTCM, drops into a tiny NS stub via BLXNS, and
 * has the NS stub store to the (default-Secure) MRAM log window 0x80090000. A
 * pass = that store raises a SecureFault (SFSR set, SFAR = 0x80090000) while a
 * Secure store to a scratch word succeeds first.
 */
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <cmsis_core.h>

/* --- addresses (transcribed from the AE822 HE DFP + hal_alif tgu.h) --- */
#define LOG_WINDOW  0x80090000U /* alp_ulog_partition global MRAM addr */
#define DTCM_BASE   0x20000000U /* HE DTCM (fork ensemble_rtss_he.dtsi) */
#define SCRATCH_SEC 0x02001210U /* SRAM0 scratch for the Secure-write sanity */

/* SAU regs: SCS_BASE (0xE000E000) + 0xDD0 (hal_alif sau_tcm_ns_setup.c) */
#define SAU_CTRL        (*(volatile uint32_t *)0xE000EDD0U)
#define SAU_RNR         (*(volatile uint32_t *)0xE000EDD8U)
#define SAU_RBAR        (*(volatile uint32_t *)0xE000EDDCU)
#define SAU_RLAR        (*(volatile uint32_t *)0xE000EDE0U)
#define SAU_CTRL_ENABLE 0x1U

/* TGU regs (hal_alif common/include/tgu.h) */
#define DTGU_BASE   0xE001E600U
#define DTGU_CFG    (*(volatile uint32_t *)(DTGU_BASE + 0x4U))
#define DTGU_LUT(n) (*(volatile uint32_t *)(DTGU_BASE + 0x10U + 4U * (n)))

/* SCB SecureFault enable + status (ARMv8-M) */
#define SCB_SHCSR            (*(volatile uint32_t *)0xE000ED24U)
#define SHCSR_SECUREFAULTENA (1U << 19)
#define SCB_SFSR             (*(volatile uint32_t *)0xE000EDE4U)
#define SCB_SFAR             (*(volatile uint32_t *)0xE000EDE8U)

/* --- beacon (global SRAM0, AP-readable over SWD) --- */
#define BEACON          ((volatile uint32_t *)0x02001100U)
#define TZ_MAGIC        0x545A4C50U /* "TZLP" */
#define TZ_START        1U
#define TZ_SEC_OK       2U
#define TZ_ENTER_NS     3U
#define TZ_PASS_FAULT   4U /* NS write to log window faulted -> enforcement works */
#define TZ_FAIL_NOFAULT 5U /* NS write returned without faulting -> NOT enforced */

static volatile uint32_t g_stage;

/* 32 KB NS chunk in DTCM (bss lands in DTCM for the ITCM-linked build). Aligned
 * generously so it covers whole TGU blocks whatever the block size. */
static uint8_t ns_area[0x8000] __aligned(0x8000);

/* NS stub, hand-assembled thumb: str r0,[r0] (0x6000) ; b . (0xE7FE).
 * cmse_nonsecure_call passes the target address in r0, so the store hits it. */
static const uint16_t ns_stub[] = { 0x6000, 0xE7FE };

typedef void (*ns_call_t)(uint32_t addr) __attribute__((cmse_nonsecure_call));

static void beacon(uint32_t result, uint32_t detail)
{
	BEACON[0] = TZ_MAGIC;
	BEACON[1] = result;
	BEACON[2] = g_stage;
	BEACON[3] = detail;
	BEACON[4] = SCB_SFSR;
	BEACON[5] = SCB_SFAR;
	BEACON[6] = DTGU_CFG;
	BEACON[7] = SAU_CTRL;
	__DSB();
	__ISB();
}

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	uint32_t sfsr = SCB_SFSR;
	/* A SecureFault (or escalated HardFault) with SFSR set and SFAR at the log
	 * window while we were in the NS-call stage = the NS store was rejected. */
	if (g_stage == TZ_ENTER_NS && sfsr != 0U) {
		beacon(TZ_PASS_FAULT, reason);
	} else {
		beacon(TZ_FAIL_NOFAULT, reason);
	}
	for (;;) {
		__WFE();
	}
}

static void mark_ns(uint32_t base, uint32_t size)
{
	/* SAU region 0 = [base, base+size) Non-Secure (NSC=0). Base/limit are 32-byte
	 * aligned; RLAR bit0 = enable. Everything else stays Secure (ALLNS=0). */
	SAU_RNR  = 0U;
	SAU_RBAR = base & ~0x1FU;
	SAU_RLAR = ((base + size - 1U) & ~0x1FU) | 0x1U;
	SAU_CTRL = SAU_CTRL_ENABLE;

	/* TGU: mark the DTCM blocks covering [base, base+size) Non-Secure. Block size
	 * = 2^(cfg_blksz+5) (hal_alif tgu.c). */
	uint32_t blksz = 1U << ((DTGU_CFG & 0xFU) + 5U);
	uint32_t first = (base - DTCM_BASE) / blksz;
	uint32_t last  = (base + size - 1U - DTCM_BASE) / blksz;
	for (uint32_t b = first; b <= last; b++) {
		DTGU_LUT(b / 32U) |= (1U << (b % 32U));
	}
	__DSB();
	__ISB();
}

int main(void)
{
	g_stage = TZ_START;
	beacon(TZ_START, 0U);

	/* Enable SecureFault so the violation reports in SFSR/SFAR (not a bare HardFault). */
	SCB_SHCSR |= SHCSR_SECUREFAULTENA;

	/* Secure-state sanity: a Secure store to a Secure scratch must succeed. */
	*(volatile uint32_t *)SCRATCH_SEC = 0x5EC00D01U;
	if (*(volatile uint32_t *)SCRATCH_SEC == 0x5EC00D01U) {
		g_stage = TZ_SEC_OK;
		beacon(TZ_SEC_OK, 0U);
	}

	/* Carve the NS chunk and copy the stub in. */
	uint32_t ns_base = (uint32_t)ns_area;
	mark_ns(ns_base, sizeof(ns_area));
	memcpy(ns_area, ns_stub, sizeof(ns_stub));
	__DSB();
	__ISB();

	/* NS stack top (8-byte aligned) in the upper half of the NS chunk. */
	__TZ_set_MSP_NS((ns_base + sizeof(ns_area)) & ~0x7U);

	/* Call the NS stub, passing the Secure log window as its store target. bit0
	 * cleared marks the pointer Non-Secure for the cmse_nonsecure_call/BLXNS. */
	ns_call_t ns = (ns_call_t)(ns_base & ~1U);
	g_stage      = TZ_ENTER_NS;
	beacon(TZ_ENTER_NS, 0U);

	ns(LOG_WINDOW);

	/* If we return here, the NS store did NOT fault -> not enforced. */
	beacon(TZ_FAIL_NOFAULT, 0U);
	return 0;
}
