/* a32/payload-isa/isa.c -- A32 ISA microbenchmark payload, the target half:
 * PMU on, core clock (PMCCNTR vs CNTVCT), the CP15 registers NS PL1 can read
 * (an UNDEF is recorded and skipped, vectors.S), then isa_bench_run() straight
 * into the NC results block (isa_bench.h). Core 0 only; core 1 parks
 * (payload_start.S). Runs once (well under 1 s) and returns to the stub.
 * Reads only: never writes a clock, ACTLR or any IMPLEMENTATION DEFINED
 * register; the FPSCR change in stage_fz() is restored and checked.
 * Reads 0x02600000..+1 MiB (FB B, clean lines only) to evict the caches.
 * Writes: its own image/.bss (0x02500000..0x025F0000, isa.ld) and the
 * results block 0x02401C00..0x02401FFF.
 */
#include <stdint.h>

#include "isa_bench.h"
#include "stub_abi.h"
#include "tr_mbox.h"

#define R ((isa_res_t *)ISA_RES_ADDR)

void payload_main(volatile tr_mbox_t *m);
void isa_install_vectors(void);
void isa_undef_record(void);

static volatile uint32_t undefs;

void isa_undef_record(void)
{
	undefs++;
}

static inline uint64_t vct(void)
{
	uint32_t lo, hi;

	__asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)::"memory");
	return (uint64_t)hi << 32 | lo;
}

static inline uint32_t pmccntr(void)
{
	uint32_t c;

	__asm__ volatile("isb\n\tmrc p15, 0, %0, c9, c13, 0" : "=r"(c)::"memory");
	return c;
}

/* One read; an UNDEF leaves ISA_REG_UNREAD and sets the register's bit. */
#define RD(i, ins) \
	do { \
		uint32_t v_ = ISA_REG_UNREAD, u_ = undefs; \
		__asm__ volatile(ins : "+r"(v_)::"memory"); \
		R->reg[ISA_##i] = v_; \
		R->undef_mask |= (undefs != u_ ? 1u : 0u) << ISA_##i; \
	} while (0)
#define RD2(i, j, ins) \
	do { \
		uint32_t lo_ = ISA_REG_UNREAD, hi_ = ISA_REG_UNREAD, u_ = undefs; \
		__asm__ volatile(ins : "+r"(lo_), "+r"(hi_)::"memory"); \
		R->reg[ISA_##i] = lo_; \
		R->reg[ISA_##j] = hi_; \
		R->undef_mask |= (undefs != u_ ? 3u : 0u) << ISA_##i; \
	} while (0)

static void read_regs(void)
{
	_Static_assert(ISA_CPUACTLR_HI == ISA_CPUACTLR_LO + 1 && ISA_CPUECTLR_HI == ISA_CPUECTLR_LO + 1,
	               "pairs");
	RD(MIDR, "mrc p15, 0, %0, c0, c0, 0");
	RD(MPIDR, "mrc p15, 0, %0, c0, c0, 5");
	RD(REVIDR, "mrc p15, 0, %0, c0, c0, 6");
	RD(SCTLR, "mrc p15, 0, %0, c1, c0, 0");
	RD(ACTLR, "mrc p15, 0, %0, c1, c0, 1");
	RD(CPACR, "mrc p15, 0, %0, c1, c0, 2");
	RD(NSACR, "mrc p15, 0, %0, c1, c1, 2");
	RD(CTR, "mrc p15, 0, %0, c0, c0, 1");
	RD(CLIDR, "mrc p15, 1, %0, c0, c0, 1");
	RD(CCSIDR_L1D, "mov %0, #0\n\tmcr p15, 2, %0, c0, c0, 0\n\tisb\n\tmrc p15, 1, %0, c0, c0, 0");
	RD(CCSIDR_L1I, "mov %0, #1\n\tmcr p15, 2, %0, c0, c0, 0\n\tisb\n\tmrc p15, 1, %0, c0, c0, 0");
	RD(CCSIDR_L2, "mov %0, #2\n\tmcr p15, 2, %0, c0, c0, 0\n\tisb\n\tmrc p15, 1, %0, c0, c0, 0");
	RD(ID_ISAR5, "mrc p15, 0, %0, c0, c2, 5");
	RD(PMCR, "mrc p15, 0, %0, c9, c12, 0");
	RD(FPSCR, "vmrs %0, fpscr");
	RD(FPEXC, "vmrs %0, fpexc");
	RD(L2CTLR, "mrc p15, 1, %0, c9, c0, 2");
	RD(L2ECTLR, "mrc p15, 1, %0, c9, c0, 3");
	RD(L2ACTLR, "mrc p15, 1, %0, c15, c0, 0");
	RD2(CPUACTLR_LO, CPUACTLR_HI, "mrrc p15, 0, %0, %1, c15");
	RD2(CPUECTLR_LO, CPUECTLR_HI, "mrrc p15, 1, %0, %1, c15");
}

void payload_main(volatile tr_mbox_t *m)
{
	volatile uint32_t *w = (volatile uint32_t *)ISA_RES_ADDR;
	uint32_t           frq, c0;
	uint64_t           t0;

	for (uint32_t i = 0; i < sizeof(isa_res_t) / 4u; i++)
		w[i] = 0;
	R->stage = 1;
	R->magic = ISA_MAGIC;
	isa_install_vectors();
	/* PMCR.E | PMCR.C, PMCNTENSET.C: the renderer's prof_enable(). */
	__asm__ volatile("mcr p15, 0, %0, c9, c12, 0\n\tmcr p15, 0, %1, c9, c12, 1\n\tisb" ::"r"(5u),
	                 "r"(0x80000000u));
	__asm__ volatile("mrc p15, 0, %0, c14, c0, 0" : "=r"(frq));
	R->cntfrq = frq;
	t0        = vct();
	c0        = pmccntr();
	while (vct() - t0 < frq / 100u) /* 10 ms */
		;
	R->clk_cyc   = pmccntr() - c0;
	R->clk_ticks = (uint32_t)(vct() - t0);
	read_regs();
	isa_bench_run(R);
	R->undef_n = undefs;
	__asm__ volatile("dsb sy" ::: "memory");
	if (m->ctrl_cmd == STUB_CMD_HALT) m->ctrl_cmd = STUB_CMD_NONE;
}
