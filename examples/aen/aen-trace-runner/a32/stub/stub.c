/* stub.c -- A32 resident stub: section table, mailbox, CPU_ON, park loops,
 * LAUNCH, re-entry, fault record. Plan sec 5; contract in stub_abi.h;
 * sequence review in IMPLEMENTATION-NOTES.md.
 *
 * Runs from SRAM1 MiB 0 (Normal NC, executable) after start.S relocates it.
 * All mutable state is either the mailbox (NC, 0x02401000) or this image's
 * .bss (also NC MiB 0) -- nothing the stub owns is ever cacheable, so the
 * two cores and the debug AP see it without maintenance.
 */
#include <stddef.h>
#include <stdint.h>

#include "crc32.h"
#include "stub_abi.h"
#include "tr_mbox.h"

_Static_assert(STUB_MBOX_ADDR == TR_MBOX_ADDR, "stub_abi.h mailbox address drifted from tr_mbox.h");
_Static_assert(offsetof(tr_mbox_t, ctrl_cmd) == MBOX_OFF_CTRL_CMD, "");
_Static_assert(offsetof(tr_mbox_t, stub_state) == MBOX_OFF_STUB_STATE, "");
_Static_assert(offsetof(tr_mbox_t, fault_core) == MBOX_OFF_FAULT_CORE, "");
_Static_assert(offsetof(tr_mbox_t, fault_code) == MBOX_OFF_FAULT_CODE, "");
_Static_assert(offsetof(tr_mbox_t, lr) == MBOX_OFF_FAULT_LR, "");
_Static_assert(offsetof(tr_mbox_t, pad4) == MBOX_OFF_LAST_FAULT_CORE, "");
_Static_assert(MBOX_OFF_LAST_FAULT_DFAR == MBOX_OFF_LAST_FAULT_CORE + 12,
               "last-fault slots are pad4[0..3]");
_Static_assert(offsetof(tr_mbox_t, pad4[TR_STUB_T_COPY0]) == MBOX_OFF_T_COPY0 &&
                   offsetof(tr_mbox_t, pad4[TR_STUB_T_JUMP]) == MBOX_OFF_T_JUMP,
               "timing stamps are pad4[4..7]");

#define MBOX ((volatile tr_mbox_t *)TR_MBOX_ADDR)

/* Section descriptor attributes (VMSAv7 short descriptor, TRE=0, AFE=0):
 * [16] S  [15] APX  [14:12] TEX  [11:10] AP  [4] XN  [3] C  [2] B  [1] 1. */
#define SEC_NC     0x00001C12u /* Normal NC, AP=11, XN */
#define SEC_NC_X   0x00001C02u /* Normal NC, AP=11, exec */
#define SEC_WB_S_X 0x00011C0Eu /* Normal WB-WA, S=1, AP=11, exec */
#define SEC_WB_S   0x00011C1Eu /* Normal WB-WA, S=1, AP=11, XN */
#define SEC_WB_RO  0x0000941Eu /* Normal WB-WA, APX=1 AP=01 (PL1 read-only), XN */
#define SEC_DEV    0x00010C16u /* Shareable Device, AP=11, XN */
#define SEC_FAULT  0x00000000u

#define CNTFRQ_HZ 100000000u /* CNTFRQ read 100000000 on silicon (probe stage 1) */

/* start.S */
void                           cpu_setup(void);
void                           mmu_on(void);
void                           cache_sync_all(void);
__attribute__((noreturn)) void payload_jump(uint32_t entry, uint32_t core);
int32_t                        psci_cpu_on(uint32_t target_mpidr, uint32_t entry);
void                           stub_core1_entry(void);
extern const uint32_t          stub_hdr_payload_off, stub_hdr_payload_len, stub_hdr_payload_crc;

/* Called from start.S only. */
void                           stub_build_table(void);
__attribute__((noreturn)) void stub_main(void);
__attribute__((noreturn)) void stub_core1_main(void);
__attribute__((noreturn)) void stub_core0_reentered(uint32_t from_fault);
__attribute__((noreturn)) void stub_core1_reentered(uint32_t from_fault);
void                           stub_fault(uint32_t code, uint32_t lr, uint32_t core);

/* Stub-private shared state (.bss, NC). core1_go is a generation counter:
 * core 0 bumps it, core 1 runs when it differs from what it last saw. */
volatile uint32_t        table_ready; /* read by start.S fault_common */
static volatile uint32_t faulting[2];
static volatile uint32_t rec_flag[2], rec_turn; /* fault-record lock */
static volatile uint32_t core1_go;
static volatile uint32_t core1_entry;

static inline void dsb(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}
static inline void wfe(void)
{
	__asm__ volatile("wfe" ::: "memory");
}
static inline void sev(void)
{
	__asm__ volatile("sev" ::: "memory");
}

static inline uint32_t cntvct_lo(void)
{
	uint32_t lo, hi;

	__asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)::"memory");
	(void)hi;
	return lo; /* 32-bit deltas: 42.9 s window, far above any wait here */
}

static inline uint32_t dcache_line(void)
{
	uint32_t ctr;

	__asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(ctr));
	return 4u << ((ctr >> 16) & 0xFu); /* CTR.DminLine, words -> bytes */
}

/* Invalidate (DCIMVAC, no writeback) or clean (DCCMVAC) [a, a+n) to PoC.
 * By-VA ops on Inner Shareable memory are broadcast to the other core. */
static void dcache_range(uint32_t a, uint32_t n, int clean)
{
	uint32_t line = dcache_line();
	uint32_t end  = a + n;

	for (a &= ~(line - 1u); a < end; a += line) {
		if (clean)
			__asm__ volatile("mcr p15, 0, %0, c7, c10, 1" ::"r"(a) : "memory");
		else
			__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a) : "memory");
	}
	dsb();
}

/* Two-core Peterson lock around the fault record: no exclusives (the page
 * is NC; LDREX/STREX are only used on Shareable WB memory in this design),
 * DSB between the flag/turn stores and the loads. Bounded at ~1 ms so a
 * core that died inside the lock cannot wedge the other. */
static void rec_lock(uint32_t core)
{
	uint32_t other = core ^ 1u, t0;

	rec_flag[core] = 1;
	rec_turn       = other;
	dsb();
	t0 = cntvct_lo();
	while (rec_flag[other] && rec_turn == other && cntvct_lo() - t0 < CNTFRQ_HZ / 1000u)
		;
}

static void rec_unlock(uint32_t core)
{
	dsb();
	rec_flag[core] = 0;
	dsb();
}

/* First fault wins: a later fault on either core never overwrites it. */
static void record_fault(uint32_t core,
                         uint32_t code,
                         uint32_t lr,
                         uint32_t dfsr,
                         uint32_t dfar,
                         uint32_t ifsr,
                         uint32_t ifar)
{
	volatile tr_mbox_t *m = MBOX;

	core &= 1u;
	rec_lock(core);
	if (m->fault_code == STUB_FAULT_NONE) {
		m->fault_core = core;
		m->dfsr       = dfsr;
		m->dfar       = dfar;
		m->ifsr       = ifsr;
		m->ifar       = ifar;
		m->lr         = lr;
		dsb();
		m->fault_code = code; /* last: a reader seeing code sees the rest */
	}
	m->stub_state = STUB_STATE_FAULT;
	rec_unlock(core);
}

/* Software fault from core 0's stub code (no CP15 fault registers). */
static void sw_fault(uint32_t code, uint32_t detail, uint32_t addr)
{
	record_fault(0, code, detail, 0, addr, 0, 0);
}

static void clear_fault(void)
{
	volatile tr_mbox_t *m = MBOX;

	m->fault_code = STUB_FAULT_NONE;
	m->fault_core = m->dfsr = m->dfar = m->ifsr = m->ifar = m->lr = 0;
}

/* ---------------------------------------------------------------- table */
void stub_build_table(void)
{
	volatile uint32_t *t = (volatile uint32_t *)STUB_TTB;

	for (uint32_t i = 0; i < 4096u; i++)
		t[i] = (i << 20) | SEC_DEV;
	for (uint32_t i = 0x020; i <= 0x023; i++) /* SRAM0: FB A/B, TF-A MHU0 window (never touched) */
		t[i] = (i << 20) | SEC_NC;
	t[0x024] = 0x02400000u | SEC_NC_X;   /* mailbox, tables, stub, stacks */
	t[0x025] = 0x02500000u | SEC_WB_S_X; /* payload image + DL/bins/stacks */
	t[0x026] = 0x02600000u | SEC_WB_S;
	t[0x027] = SEC_FAULT;               /* TF-A RW 0x027DE000-0x027ED000 */
	t[0x800] = 0x80000000u | SEC_WB_RO; /* MRAM: release payload source only */
	dsb();
	table_ready = 1;
}

/* --------------------------------------------------------------- launch */
static uint32_t range_ok(uint32_t entry, uint32_t len)
{
	return (entry & 3u) == 0 && entry >= STUB_PAYLOAD_BASE && entry < STUB_PAYLOAD_LIMIT &&
	       len != 0 && len <= STUB_PAYLOAD_LIMIT - entry;
}

/* Returns only when the LAUNCH is refused (fault recorded). */
static void launch(void)
{
	volatile tr_mbox_t *m     = MBOX;
	uint32_t            entry = m->ctrl_entry, len = m->ctrl_len, want = m->ctrl_crc;

	m->pad4[TR_STUB_T_JUMP]   = 0; /* set again only if this LAUNCH jumps */
	m->pad4[TR_STUB_T_LAUNCH] = cntvct_lo();
	if (m->stub_core1_state == STUB_CORE1_RUNNING) {
		sw_fault(STUB_FAULT_CORE1_BUSY, 0, entry);
		return;
	}
	if (!range_ok(entry, len)) {
		sw_fault(STUB_FAULT_BAD_RANGE, len, entry);
		return;
	}
	/* The debug AP / M55 wrote the image to memory behind our caches. Drop
	 * any cached copy (clean lines only: every return path ran DCCISW)
	 * before the CRC reads it. */
	dcache_range(entry, len, 0);
	uint32_t got = tr_crc32(0, (const void *)entry, len);
	if (got != want) {
		sw_fault(STUB_FAULT_BAD_CRC, got, entry);
		return;
	}
	clear_fault();
	/* A fresh, non-zero launch token for the payload's core-1 gate: the self-launch never
	 * visits the park loop that used to be the only thing bumping it. */
	m->stub_heartbeat0 = stub_next_token(m->stub_heartbeat0);
	cache_sync_all(); /* DCCISW all levels, ICIALLU, BPIALL, DSB, ISB */
	m->pad4[TR_STUB_T_JUMP] = cntvct_lo();
	m->stub_state           = STUB_STATE_RUNNING;
	if (m->stub_core1_state == STUB_CORE1_PARKED) {
		core1_entry         = entry;
		m->stub_core1_state = STUB_CORE1_RUNNING; /* core 0 owns PARKED -> RUNNING */
		dsb();
		core1_go = core1_go + 1u;
		dsb();
		sev();
	}
	dsb();
	payload_jump(entry, 0);
}

/* ------------------------------------------------------------ park loops */
static __attribute__((noreturn)) void park0(void)
{
	volatile tr_mbox_t *m = MBOX;

	faulting[0] = 0;
	for (;;) {
		m->stub_heartbeat0 = m->stub_heartbeat0 + 1u;
		uint32_t cmd       = m->ctrl_cmd;
		if (cmd != STUB_CMD_NONE) {
			m->ctrl_cmd = STUB_CMD_NONE; /* consumer clears; HALT while parked is a no-op */
			dsb();
			if (cmd == STUB_CMD_LAUNCH) launch();
		}
		wfe();
	}
}

/* `seen` is sampled BEFORE the caller publishes PARKED: core 0 may bump
 * core1_go as soon as it sees PARKED, and that bump must not be missed. */
static __attribute__((noreturn)) void park1(uint32_t seen)
{
	volatile tr_mbox_t *m = MBOX;

	faulting[1] = 0;
	for (;;) {
		m->stub_heartbeat1 = m->stub_heartbeat1 + 1u;
		if (core1_go != seen) {
			dsb();
			uint32_t entry = core1_entry;
			/* Core 0's set/way + ICIALLU covered only core 0: redo the
			 * I-side here. D-side is coherent (S=1, SMPEN) and the image
			 * reached PoC via core 0's DCIMVAC/DCCISW. */
			__asm__ volatile("mcr p15, 0, %0, c7, c5, 0\n\t" /* ICIALLU */
			                 "mcr p15, 0, %0, c7, c5, 6\n\t" /* BPIALL */
			                 "dsb sy\n\tisb" ::"r"(0)
			                 : "memory");
			payload_jump(entry, 1);
		}
		wfe();
	}
}

/* ---------------------------------------------------------------- entry */
void stub_main(void)
{
	volatile tr_mbox_t *m = MBOX;

	/* Mailbox init: identity + a clean control/fault state. ctrl_entry/len/
	 * crc are deliberately kept (plan sec 5 step 3). A ctrl_cmd left over
	 * from before this boot is dropped rather than obeyed. The previous
	 * boot's fault record survives in pad4[0..3] (MBOX_OFF_LAST_FAULT_*) when
	 * the magic says the page was ours; cold SRAM is garbage, so zero then. */
	uint32_t warm = (uint32_t)tr_mbox_stub_page_init(m); /* cold or old-version: cleared */
	m->pad4[0]    = warm ? m->fault_core : 0;
	m->pad4[1]    = warm ? m->fault_code : 0;
	m->pad4[2]    = warm ? m->lr : 0;
	m->pad4[3]    = warm ? m->dfar : 0;
	for (uint32_t i = TR_STUB_T_COPY0; i <= TR_STUB_T_JUMP; i++)
		m->pad4[i] = 0;
	m->stub_state       = STUB_STATE_PARKED;
	m->stub_core1_state = STUB_CORE1_OFF;
	m->stub_heartbeat0 = m->stub_heartbeat1 = 0;
	m->ctrl_cmd                             = STUB_CMD_NONE;
	clear_fault();
	m->version = TR_MBOX_VERSION;
	dsb();
	m->magic = TR_MBOX_MAGIC;
	dsb();

	/* Core 1: target MPIDR 0x00000001 (Aff0=1, bit31 masked -- never the
	 * raw MPIDR). Then wait up to 100 ms for it to park so a LAUNCH (or the
	 * release self-LAUNCH below) sees a definite core-1 state. */
	int32_t rc = psci_cpu_on(0x00000001u, (uint32_t)stub_core1_entry);
	if (rc != 0) {
		sw_fault(STUB_FAULT_PSCI, (uint32_t)rc, 0);
	} else {
		uint32_t t0 = cntvct_lo();
		while (m->stub_core1_state != STUB_CORE1_PARKED && cntvct_lo() - t0 < CNTFRQ_HZ / 10u)
			wfe();
		if (m->stub_core1_state != STUB_CORE1_PARKED) sw_fault(STUB_FAULT_PSCI, 1, 0);
	}

	/* Release mode: payload appended to this image in MRAM. */
	uint32_t off = stub_hdr_payload_off, len = stub_hdr_payload_len;
	if (len != 0) {
		if (off < 0x40u || (off & 3u) || off > STUB_MRAM_MAPPED_END - STUB_MRAM_BASE ||
		    len > STUB_MRAM_MAPPED_END - STUB_MRAM_BASE - off ||
		    len > STUB_PAYLOAD_LIMIT - STUB_PAYLOAD_BASE) {
			sw_fault(STUB_FAULT_BAD_HEADER, len, off);
		} else {
			const volatile uint32_t *src = (const volatile uint32_t *)(STUB_MRAM_BASE + off);
			volatile uint32_t       *dst = (volatile uint32_t *)STUB_PAYLOAD_BASE;
			m->pad4[TR_STUB_T_COPY0]     = cntvct_lo();
			for (uint32_t i = 0; i < (len + 3u) / 4u; i++)
				dst[i] = src[i];
			/* Clean: launch() INVALIDATES the range before its CRC, which
			 * would otherwise discard this copy. */
			dcache_range(STUB_PAYLOAD_BASE, len, 1);
			m->pad4[TR_STUB_T_COPY1] = cntvct_lo();
			m->ctrl_entry            = STUB_PAYLOAD_BASE;
			m->ctrl_len              = len;
			m->ctrl_crc              = stub_hdr_payload_crc;
			dsb();
			launch(); /* returns only if refused */
		}
	}
	park0();
}

void stub_core1_main(void)
{
	volatile tr_mbox_t *m = MBOX;

	uint32_t seen = core1_go;

	dsb();
	m->stub_core1_state = STUB_CORE1_PARKED;
	dsb();
	sev();
	park1(seen);
}

/* ------------------------------------------------------------- re-entry */
void stub_core0_reentered(uint32_t from_fault)
{
	volatile tr_mbox_t *m  = MBOX;
	uint32_t            t0 = cntvct_lo();

	(void)from_fault;
	/* Core 1 must finish its own set/way pass before ours touches the
	 * shared L2 -- and must be out of the payload before it is replaced. */
	while (m->stub_core1_state == STUB_CORE1_RUNNING && cntvct_lo() - t0 < CNTFRQ_HZ)
		wfe();
	if (m->stub_core1_state == STUB_CORE1_RUNNING) sw_fault(STUB_FAULT_CORE1_TIMEOUT, 0, 0);
	cache_sync_all();
	if (m->ctrl_cmd == STUB_CMD_HALT) m->ctrl_cmd = STUB_CMD_NONE;
	if (m->stub_state == STUB_STATE_RUNNING) m->stub_state = STUB_STATE_PARKED;
	dsb();
	park0();
}

void stub_core1_reentered(uint32_t from_fault)
{
	volatile tr_mbox_t *m = MBOX;

	(void)from_fault;
	cache_sync_all();
	uint32_t seen = core1_go;
	dsb();
	m->stub_core1_state = STUB_CORE1_PARKED;
	dsb();
	sev();
	park1(seen);
}

/* --------------------------------------------------------------- faults */
/* Called by start.S fault_common in SVC, already back on the stub table. */
void stub_fault(uint32_t code, uint32_t lr, uint32_t core)
{
	volatile tr_mbox_t *m = MBOX;
	uint32_t            dfsr, dfar, ifsr, ifar;

	core &= 1u;
	__asm__ volatile("mrc p15, 0, %0, c5, c0, 0\n\t"
	                 "mrc p15, 0, %1, c6, c0, 0\n\t"
	                 "mrc p15, 0, %2, c5, c0, 1\n\t"
	                 "mrc p15, 0, %3, c6, c0, 2"
	                 : "=r"(dfsr), "=r"(dfar), "=r"(ifsr), "=r"(ifar));
	record_fault(core, code, lr, dfsr, dfar, ifsr, ifar);
	m->ctrl_cmd = STUB_CMD_HALT; /* the sibling's payload sees HALT and returns */
	dsb();
	sev();
	/* No table yet, or this core faulted again before reaching its park
	 * loop (the re-entry path itself faults): stop here, in SRAM1. Core 1
	 * reports OFF so core 0 neither waits for it nor refuses LAUNCH as
	 * CORE1_BUSY -- the next LAUNCH runs single-core. Core 0 stays FAULT;
	 * only a reset recovers it (nothing else could service LAUNCH). */
	if (!table_ready || faulting[core]) {
		if (core == 1u) {
			m->stub_core1_state = STUB_CORE1_OFF;
			dsb();
			sev();
		}
		for (;;)
			wfe();
	}
	faulting[core] = 1;
}
