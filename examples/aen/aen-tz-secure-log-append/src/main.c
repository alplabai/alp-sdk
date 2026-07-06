/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Single-core TrustZone-M END-TO-END enforced append (no second core).
 *
 * The Non-Secure application cannot write the log store (SAU marks it Secure ->
 * AttributionUnit Violation). It APPENDS by writing a request into a shared
 * Non-Secure mailbox and returning to the Secure owner, which reads the request
 * and performs the privileged log write. This is the single-core mirror of the
 * two-core HP-owner / HE-client split: the owner is the Secure world, the client
 * is the Non-Secure app, and the request channel is shared NS memory serviced on
 * the return from the app.
 *
 * Uses only the S->NS call direction (cmse_nonsecure_call / BLXNS, which returns
 * via BX LR) -- it does NOT need an NS->Secure callback (NSC veneer), which Alif
 * silicon does not attribute for a custom window.
 *
 * Pass = the owner appended the app's value on its behalf (count 0->1, value in
 * the Secure store) AND the app's own direct store to the log raised SFSR.AUVIOL.
 */
#include <zephyr/kernel.h>
#include <zephyr/fatal.h>
#include <cmsis_core.h>

#define DTCM_BASE 0x20000000U
#define MBOX_OFF  0x100U /* request mailbox offset inside the NS chunk */
#define APP_VALUE 0xC0DE1234U

#define SAU_CTRL (*(volatile uint32_t *)0xE000EDD0U)
#define SAU_RNR  (*(volatile uint32_t *)0xE000EDD8U)
#define SAU_RBAR (*(volatile uint32_t *)0xE000EDDCU)
#define SAU_RLAR (*(volatile uint32_t *)0xE000EDE0U)

#define DTGU_BASE   0xE001E600U
#define DTGU_CFG    (*(volatile uint32_t *)(DTGU_BASE + 0x4U))
#define DTGU_LUT(n) (*(volatile uint32_t *)(DTGU_BASE + 0x10U + 4U * (n)))

#define SCB_SHCSR            (*(volatile uint32_t *)0xE000ED24U)
#define SHCSR_SECUREFAULTENA (1U << 19)
#define SCB_SFSR             (*(volatile uint32_t *)0xE000EDE4U)
#define SCB_SFAR             (*(volatile uint32_t *)0xE000EDE8U)

#define BEACON   ((volatile uint32_t *)0x02001100U)
#define TZ_MAGIC 0x545A4C41U /* "TZLA" */
#define RES_PASS 4U
#define RES_FAIL 5U

static volatile uint32_t g_stage;
enum { ST_REQ = 1, ST_ATTACK = 3 };

/* Secure log store (owner-only). Default-Secure. */
static volatile uint32_t sec_log[8];
static volatile uint32_t sec_cnt;

/* 32 KB Non-Secure chunk: app stubs + stack + the request mailbox. */
static uint8_t ns_area[0x8000] __aligned(0x8000);

/* NS "request" stub: str r1,[r0] ; bx lr  -> writes val to the NS mailbox, returns. */
static const uint16_t ns_request_stub[] = { 0x6001, 0x4770 };
/* NS "attack" stub: str r0,[r0] ; b .     -> illegal direct store to the Secure log. */
static const uint16_t ns_attack_stub[] = { 0x6000, 0xE7FE };

typedef void (*ns_req_t)(uint32_t mbox, uint32_t val) __attribute__((cmse_nonsecure_call));
typedef void (*ns_atk_t)(uint32_t log) __attribute__((cmse_nonsecure_call));

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
	ARG_UNUSED(esf);
	ARG_UNUSED(reason);
	uint32_t sfsr = SCB_SFSR;
	BEACON[3]     = (g_stage == ST_ATTACK && (sfsr & (1U << 3))) ? RES_PASS : RES_FAIL;
	BEACON[4]     = sfsr;
	BEACON[5]     = SCB_SFAR;
	__DSB();
	__ISB();
	for (;;) {
		__WFE();
	}
}

int main(void)
{
	BEACON[0] = TZ_MAGIC;
	BEACON[1] = 0;
	BEACON[3] = 0;
	SCB_SHCSR |= SHCSR_SECUREFAULTENA;

	uint32_t ns_base = (uint32_t)ns_area;

	/* SAU region 0: the NS chunk = Non-Secure; the log store stays Secure. */
	SAU_RNR  = 0U;
	SAU_RBAR = ns_base & ~0x1FU;
	SAU_RLAR = ((ns_base + sizeof(ns_area) - 1U) & ~0x1FU) | 0x1U;
	SAU_CTRL = 0x1U;

	/* TGU: the NS DTCM blocks -> Non-Secure. */
	uint32_t blksz = 1U << ((DTGU_CFG & 0xFU) + 5U);
	uint32_t first = (ns_base - DTCM_BASE) / blksz;
	uint32_t last  = (ns_base + sizeof(ns_area) - 1U - DTCM_BASE) / blksz;
	for (uint32_t b = first; b <= last; b++) {
		DTGU_LUT(b / 32U) |= (1U << (b % 32U));
	}
	__DSB();
	__ISB();

	memcpy(ns_area + 0x00U, ns_request_stub, sizeof(ns_request_stub));
	memcpy(ns_area + 0x40U, ns_attack_stub, sizeof(ns_attack_stub));
	*(volatile uint32_t *)(ns_base + MBOX_OFF) = 0U;
	__DSB();
	__ISB();
	__TZ_set_MSP_NS((ns_base + sizeof(ns_area)) & ~0x7U);

	/* Phase 1 (app): write an append request into the NS mailbox, then return. */
	ns_req_t ns_req = (ns_req_t)((ns_base + 0x00U) & ~1U);
	g_stage         = ST_REQ;
	ns_req(ns_base + MBOX_OFF, APP_VALUE);

	/* Owner (Secure): service the request -> the privileged log write the app
	 * itself is not allowed to do. */
	uint32_t req          = *(volatile uint32_t *)(ns_base + MBOX_OFF);
	sec_log[sec_cnt & 7U] = req;
	sec_cnt++;
	BEACON[1] = sec_cnt;    /* appended count (0->1) */
	BEACON[2] = sec_log[0]; /* value the owner wrote on the app's behalf */
	__DSB();

	/* Phase 2 (app): attempt the illegal direct store to the Secure log. */
	ns_atk_t ns_atk = (ns_atk_t)((ns_base + 0x40U) & ~1U);
	g_stage         = ST_ATTACK;
	ns_atk((uint32_t)sec_log);

	BEACON[3] = RES_FAIL; /* reached only if the illegal store did not fault */
	return 0;
}
