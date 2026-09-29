/* a32/payload-isa/isa_bench.h -- A32 ISA microbenchmark results block.
 *
 * Answers the questions docs/superpowers/specs/2026-09-23-a32-isa-opportunities.md
 * cannot settle by reading: VDIV vs VRECPE/VRECPS, NEON vs scalar Gouraud
 * spans by length, edge-walk variants, PLD on the texture gather, FPSCR
 * FZ/DN on denormals, band-fill store forms, the golden raster at the build's
 * -O level, the core clock, and which implementation-defined registers NS
 * PL1 can read.
 *
 * Block: ISA_RES_ADDR, NC (mailbox page tail, 0x02401C00..0x02401FFF; free:
 * tr_mbox_t 0x000-0x1FF, prof 0x800-0x8DF, stats 0x900-0x93F, accel probe
 * 0x02401A00-0x02401A8F). Read: `mem32 0x02401C00, 256`, decode with
 * decode_isa.py, which parses the X-lists below -- this header is the only
 * copy of the layout.
 */
#ifndef ISA_BENCH_H
#define ISA_BENCH_H

#include <stdint.h>

#define ISA_RES_ADDR  0x02401C00u
#define ISA_MAGIC     0x15A0B001u
#define ISA_STAGE_END 0xD0Eu

/* Registers read from NS PL1; a read that UNDEFs sets its bit in undef_mask
 * and leaves ISA_REG_UNREAD. */
#define ISA_REG_UNREAD 0xBADC0DE5u
/* clang-format off */
#define ISA_REGS(X) \
	X(MIDR) X(MPIDR) X(REVIDR) X(SCTLR) X(ACTLR) X(CPACR) X(NSACR) X(CTR) \
	X(CLIDR) X(CCSIDR_L1D) X(CCSIDR_L1I) X(CCSIDR_L2) X(ID_ISAR5) X(PMCR) \
	X(FPSCR) X(FPEXC) X(L2CTLR) X(L2ECTLR) X(L2ACTLR) X(CPUACTLR_LO) \
	X(CPUACTLR_HI) X(CPUECTLR_LO) X(CPUECTLR_HI)

/* Timings: {cycles (PMCCNTR), units}; the decoder prints cycles / unit.
 * GSPAN: a 720-px constant-w fogged ground row through the generic sub-span
 * loop (OLD) vs span_tex()'s hoisted one (CW).
 * Units: ops for the FP rows, px for spans / tex / golden raster, rows for
 * the edge walk, bytes for fills, reads/hooks for the PMU rows, divides for
 * UDIV/LDIV64/RECIP62, triangles for TRI_SETUP. */
#define ISA_TIMINGS(X) \
	X(PMU_READ) \
	X(PROF_HOOK) \
	X(FDIV_LAT) X(FDIV_TPUT) X(RECP_LAT) X(RECP_TPUT) X(RECP_Q4_LAT) X(RECP_Q4_TPUT) X(FMUL_NORM) \
	    X(FMUL_DENORM) X(FMUL_NORM_FZ) X(FMUL_DENORM_FZ) X(GZ_CUR_4) X(GZ_CUR_8) X(GZ_CUR_9) \
	        X(GZ_CUR_12) X(GZ_CUR_16) X(GZ_CUR_64) X(GZ_SCALAR_4) X(GZ_SCALAR_8) X(GZ_SCALAR_9) \
	            X(GZ_SCALAR_12) X(GZ_SCALAR_16) X(GZ_SCALAR_64) X(GZ_MASK_4) X(GZ_MASK_8) \
	                X(GZ_MASK_9) X(GZ_MASK_12) X(GZ_MASK_16) X(GZ_MASK_64) X(EDGE_CUR) X(EDGE_REG) \
	                    X(EDGE_FIX) X(TEX_WARM) X(TEX_WARM_PLD) X(TEX_COLD) X(TEX_COLD_PLD) \
	                        X(GSPAN_OLD) X(GSPAN_CW) X(FILL16K_MEMSET) X(FILL16K_VST1Q) \
	                            X(FILL16K_VST256) X(FILL16K_VSTM) X(FILL16K_STRD) \
	                                X(FILL45K_MEMSET) X(FILL45K_VST1Q) X(FILL45K_VST256) \
	                                    X(FILL45K_VSTM) X(FILL45K_STRD) X(UDIV_LAT) X(LDIV64) \
	                                        X(RECIP62_FAST) X(TRI_SETUP) X(GOLD_SETUP) \
	                                            X(GOLD_RASTER)

/* Scalar results. */
#define ISA_VALS(X) \
	X(RECP_MISMATCH) \
	X(RECP_MAX_ULP) \
	X(RECP_SAMPLES) X(EDGE_FIX_MISMATCH) X(EDGE_ROWS) X(GOLD_TRIS) X(GOLD_BANDS_OK) X(GZ_SPANS)

/* Checks: pass bit set when it held, fail bit when it did not. */
#define ISA_CHECKS(X) \
	X(GZ_CUR_EXACT) \
	X(GZ_MASK_EXACT) \
	X(EDGE_REG_EXACT) X(TEX_PLD_EXACT) X(FILL_OK) X(GOLDEN_OK) X(FDIV_OK) X(FZ_RESTORED) \
	    X(RECIP62_EXACT) X(GSPAN_EXACT)

/* clang-format on */
#define ISA_ENUM(n) ISA_##n,
enum { ISA_REGS(ISA_ENUM) ISA_REG_N };
enum { ISA_TIMINGS(ISA_ENUM) ISA_T_N };
enum { ISA_VALS(ISA_ENUM) ISA_V_N };
enum { ISA_CHECKS(ISA_ENUM) ISA_C_N };
#undef ISA_ENUM

typedef struct {
	uint32_t cyc, units;
} isa_t;

typedef struct {
	uint32_t magic;      /* +0x00 ISA_MAGIC once the block is valid */
	uint32_t stage;      /* +0x04 last stage started (1..), ISA_STAGE_END when done */
	uint32_t opt;        /* +0x08 build: -O level (ISA_OPT) | 0x100 -funroll-loops */
	uint32_t pass, fail; /* +0x0C, +0x10 ISA_CHECKS bits */
	uint32_t cntfrq;     /* +0x14 */
	uint32_t clk_cyc;    /* +0x18 PMCCNTR delta over ... */
	uint32_t clk_ticks;  /* +0x1C ... this many CNTVCT ticks */
	uint32_t undef_n;    /* +0x20 UNDEF exceptions taken */
	uint32_t undef_mask; /* +0x24 ISA_REGS bits that UNDEFed */
	uint32_t reg[ISA_REG_N];
	uint32_t val[ISA_V_N];
	isa_t    t[ISA_T_N];
} isa_res_t;

_Static_assert(sizeof(isa_res_t) <= 0x400,
               "results block must end at the mailbox page's L2 tables (0x02402000)");

/* Portable driver: runs every stage into *r (host / qemu: timings 0). */
void isa_bench_run(isa_res_t *r);

#endif /* ISA_BENCH_H */
