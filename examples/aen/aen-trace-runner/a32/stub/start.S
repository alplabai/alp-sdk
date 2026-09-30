/* start.S -- A32 resident stub: reset, relocation, per-core CPU/MMU setup,
 * set/way maintenance, PSCI, payload jump, re-entry and fault vectors.
 *
 * Lineage: a32/probe/start.S (silicon-proven 2026-09-22) -- the DCCISW loop,
 * SCTLR forcing, CPACR/FPEXC and CNTKCTL sequences are that file's, moved
 * into callable routines. What changed and why: IMPLEMENTATION-NOTES.md.
 *
 * Linked at STUB_BASE (0x0240C000, SRAM1 MiB 0) with its load image at
 * STUB_MRAM_BASE (0x80020000). Everything in .vectors runs at BOTH addresses
 * (MRAM before the copy, SRAM after), so it is position independent: `b`,
 * `adr`, literal-pool loads of absolute constants. Everything in .text runs
 * only from SRAM.
 */
#include "stub_abi.h"

	.syntax unified
	.arm

	.equ SCTLR_M,    (1 << 0)
	.equ SCTLR_A,    (1 << 1)
	.equ SCTLR_C,    (1 << 2)
	.equ SCTLR_Z,    (1 << 11)
	.equ SCTLR_I,    (1 << 12)
	.equ SCTLR_V,    (1 << 13)
	.equ SCTLR_WXN,  (1 << 19)
	.equ SCTLR_UWXN, (1 << 20)
	.equ SCTLR_EE,   (1 << 25)
	.equ SCTLR_TRE,  (1 << 28)
	.equ SCTLR_AFE,  (1 << 29)
	.equ SCTLR_TE,   (1 << 30)
	.equ MODE_SVC,   0x13

	.section .vectors, "ax"
	.global _vectors
_vectors:
	b   reset_handler            /* 0x00 */
	b   undef_vec                /* 0x04 */
	b   unexpected_vec           /* 0x08 SVC */
	b   pabort_vec               /* 0x0C */
	b   dabort_vec               /* 0x10 */
	b   unexpected_vec           /* 0x14 reserved */
	b   unexpected_vec           /* 0x18 IRQ (masked) */
	b   unexpected_vec           /* 0x1C FIQ (masked) */
	/* 0x20 / 0x24: the payload return ABI (STUB_REENTER_CORE0/1). Fixed
	 * offsets -- stub_abi.h and every payload depend on them. */
	b   stub_reenter_core0
	b   stub_reenter_core1
	/* 0x28..0x37: release header (STUB_HDR_*), patched by mkpayload.py. */
	.global stub_hdr_payload_off, stub_hdr_payload_len, stub_hdr_payload_crc
stub_hdr_payload_off: .word 0
stub_hdr_payload_len: .word 0      /* 0 = dev build: never self-LAUNCH */
stub_hdr_payload_crc: .word 0
	.word STUB_HDR_MARK_VALUE

/* ---------------------------------------------------------------------------
 * reset_handler -- TF-A sp_min erets here (0x80020000): AArch32 NS SVC,
 * A/I/F masked, MMU off. Copy the loaded image to STUB_BASE, zero .bss,
 * jump there. From then on this core never fetches from MRAM (a Flow D write
 * can rewrite MRAM under a parked stub without tearing it).
 * ------------------------------------------------------------------------- */
reset_handler:
	adr r0, _vectors                @ runtime address (0x80020000)
	mcr p15, 0, r0, c12, c0, 0      @ VBAR -> MRAM vectors until relocated
	mrc p15, 0, r1, c1, c0, 0
	bic r1, r1, #SCTLR_V            @ V=1 would ignore VBAR
	bic r1, r1, #SCTLR_TE           @ TE=1 would take exceptions in Thumb: our vectors are ARM
	mcr p15, 0, r1, c1, c0, 0
	isb
	ldr sp, =STUB_STACK0_TOP

	/* Warm TF-A handoff: clean+invalidate every D-cache level by set/way
	 * BEFORE the copy (probe review F1: DCCISW, never DCISW). Doing it first
	 * means no dirty line left over from earlier software can be evicted
	 * later on top of the freshly copied stub. cache_sync_all lives in .text
	 * but is position independent, so `bl` reaches its MRAM copy here. */
	push {r0}
	bl  cache_sync_all
	pop {r0}

	/* copy [runtime _vectors, +image size) -> link _vectors. MMU off: data
	 * accesses are Device-typed and uncached, so the stores reach SRAM;
	 * word loop (image size is 16-aligned by link.ld, addresses aligned). */
	ldr r1, =_vectors               @ link address, STUB_BASE
	ldr r2, =__load_end
1:	ldr r3, [r0], #4
	str r3, [r1], #4
	cmp r1, r2
	blo 1b
	ldr r1, =__bss_start
	ldr r2, =__bss_end
	mov r3, #0
2:	cmp r1, r2
	strlo r3, [r1], #4
	blo 2b
	dsb sy
	mov r0, #0
	mcr p15, 0, r0, c7, c5, 0       @ ICIALLU: the new SRAM code must not hit stale I-lines
	mcr p15, 0, r0, c7, c5, 6       @ BPIALL
	dsb sy
	isb
	ldr pc, =relocated              @ absolute jump MRAM -> SRAM (~2 GiB, not a `b`)

/* ---------------------------------------------------------------------------
 * Fault vectors. Shared by both cores and by the MRAM (pre-relocation) and
 * SRAM copies. r0 = code, r1 = banked LR of the exception mode.
 * ------------------------------------------------------------------------- */
undef_vec:
	mov r0, #STUB_FAULT_UNDEF
	b   fault_common
pabort_vec:
	mov r0, #STUB_FAULT_PABORT
	b   fault_common
dabort_vec:
	mov r0, #STUB_FAULT_DABORT
	b   fault_common
unexpected_vec:
	mov r0, #STUB_FAULT_UNEXPECTED
fault_common:
	mov r1, lr
	mrc p15, 0, r2, c0, c0, 5
	and r2, r2, #0xFF               @ r2 = core id (MPIDR.Aff0)
	adr r3, fault_common
	cmp r3, #0x80000000
	bhs early_fault                 @ still running from MRAM: park in SRAM0 instead
	cpsid aif, #MODE_SVC            @ SVC: the exception mode's banked SP is unset
	mov r4, r0
	mov r5, r1
	mov r6, r2
	/* Back onto the STUB table before touching a stack or the mailbox: the
	 * fault may come from a payload whose own table is live. mmu_on needs no
	 * stack (clobbers r0 only). Skipped while the table does not exist yet
	 * (MMU is still off then). `table_ready` sits in the stub .bss, 0x024xxxxx,
	 * which every payload table maps VA==PA (contract). */
	ldr r3, =table_ready
	ldr r3, [r3]
	cmp r3, #0
	blne mmu_on
	cmp r6, #0
	ldreq sp, =STUB_STACK0_TOP
	ldrne sp, =STUB_STACK1_TOP
	mov r0, r4
	mov r1, r5
	mov r2, r6
	bl  stub_fault                  @ records; returns only if re-entry is safe
	cmp r6, #0
	beq reenter0_common
	b   reenter1_common

	/* Fault while still executing from MRAM (reset, the pre-copy DCCISW, the
	 * copy itself -- e.g. SRAM1 not writable). Never park in MRAM: a Flow D
	 * write may rewrite it under us. Install a 10-word park page in SRAM0 at
	 * STUB_EARLY_PARK (MMU off: plain physical memory, the probe's park
	 * address), point VBAR at it, record there first (reachable even when
	 * SRAM1 is not), then try the mailbox (a fault there now lands in the
	 * SRAM0 loop), then park in SRAM0. No heartbeat. */
early_fault:
	adr r3, early_fault
	cmp r1, r3
	blo 4f
	adr r3, early_fault_end
	cmp r1, r3
	blo early_last_resort           @ faulted inside this handler (SRAM0 unwritable)
4:	adr r3, early_park_tpl
	ldr r4, =STUB_EARLY_PARK
	mov r5, #EARLY_PARK_TPL_WORDS
5:	ldr r6, [r3], #4
	str r6, [r4], #4
	subs r5, r5, #1
	bne 5b
	ldr r4, =STUB_EARLY_PARK
	ldr r3, =STUB_EARLY_MAGIC
	str r3, [r4, #0x40]
	str r2, [r4, #0x44]             @ core
	str r0, [r4, #0x48]             @ code
	str r1, [r4, #0x4C]             @ lr
	dsb sy
	mov r3, #0
	mcr p15, 0, r3, c7, c5, 0       @ ICIALLU (fresh code in SRAM0)
	mcr p15, 0, r4, c12, c0, 0      @ VBAR -> SRAM0 park page
	isb
	ldr r3, =STUB_MBOX_ADDR
	str r2, [r3, #MBOX_OFF_FAULT_CORE]
	str r0, [r3, #MBOX_OFF_FAULT_CODE]
	str r1, [r3, #MBOX_OFF_FAULT_LR]
	mov r0, #STUB_STATE_FAULT
	str r0, [r3, #MBOX_OFF_STUB_STATE]
	dsb sy
	add pc, r4, #0x20               @ park in SRAM0
early_last_resort:                  @ no writable RAM at all: nothing better exists
	wfe
	b   early_last_resort
early_fault_end:

	/* Copied to STUB_EARLY_PARK: 8 vectors -> loop at +0x20. Relative
	 * branches only, so the copy behaves like the original. */
early_park_tpl:
	.rept 8
	b   early_park_loop
	.endr
early_park_loop:
	wfe
	b   early_park_loop
	.equ EARLY_PARK_TPL_WORDS, (. - early_park_tpl) / 4

	.ltorg

/* ======================================================================== */
	.text

/* Core 0 after relocation: running from SRAM, MMU still off. */
relocated:
	bl  cpu_setup                   @ VBAR=SRAM, NEON, FPSCR=0, CNTKCTL
	bl  stub_build_table            @ C; NEON already on, so -O2 vector code is safe
	bl  mmu_on
	b   stub_main                   @ C, noreturn

/* PSCI CPU_ON target for core 1 (entered MMU off, SVC, masked). */
	.global stub_core1_entry
stub_core1_entry:
	ldr sp, =STUB_STACK1_TOP
	bl  cpu_setup
	bl  mmu_on                      @ same table core 0 built; no set/way here (see notes)
	b   stub_core1_main             @ C, noreturn

/* Payload return points (reached via the b at 0x20 / 0x24). */
stub_reenter_core0:
	mov r4, #0                      @ r4: 0 = normal return, 1 = from fault
	b   1f
reenter0_common:
	mov r4, #1
1:	cpsid aif, #MODE_SVC
	ldr sp, =STUB_STACK0_TOP
	bl  cpu_setup
	bl  mmu_on                      @ TTBR0 back to STUB_TTB, TLBIALL
	mov r0, r4
	b   stub_core0_reentered        @ C: waits core 1, cache_sync_all, park

stub_reenter_core1:
	mov r4, #0
	b   2f
reenter1_common:
	mov r4, #1
2:	cpsid aif, #MODE_SVC
	ldr sp, =STUB_STACK1_TOP
	bl  cpu_setup
	bl  mmu_on
	mov r0, r4
	b   stub_core1_reentered

/* ---------------------------------------------------------------------------
 * cpu_setup -- per-PE state every entry path needs (all banked per core):
 * VBAR -> SRAM vectors, SCTLR.V=0 and TE=0 (ARM-state vectors), NEON (CPACR cp10/11, FPEXC.EN),
 * FPSCR = 0 (RN, FZ=0, DN=0: determinism ruling), CNTKCTL event stream
 * (EVNTEN, EVNTI=15 -- without it WFE never wakes: no GIC, all masked).
 * Clobbers r0.
 * ------------------------------------------------------------------------- */
	.global cpu_setup
cpu_setup:
	ldr r0, =_vectors
	mcr p15, 0, r0, c12, c0, 0      @ VBAR
	mrc p15, 0, r0, c1, c0, 0
	bic r0, r0, #SCTLR_V
	bic r0, r0, #SCTLR_TE
	mcr p15, 0, r0, c1, c0, 0
	mrc p15, 0, r0, c1, c0, 2
	orr r0, r0, #(0xF << 20)        @ CPACR cp10/cp11 full access
	mcr p15, 0, r0, c1, c0, 2
	isb
	mov r0, #0x40000000
	vmsr fpexc, r0                  @ FPEXC.EN (UNDEFs if NS FP access is denied)
	mov r0, #0
	vmsr fpscr, r0
	mov r0, #((15 << 4) | (1 << 2)) @ CNTKCTL: EVNTI=15, EVNTEN=1
	mcr p15, 0, r0, c14, c1, 0
	isb
	bx  lr

/* ---------------------------------------------------------------------------
 * mmu_on -- point this core at STUB_TTB and make sure M/C/I/Z are on with
 * the attribute-interpretation bits forced (probe review F5). Valid with the
 * MMU off (enables it) and on (switches tables): every caller executes from
 * 0x024xxxxx, which is VA == PA in both the stub table and any payload table
 * that honours the contract. Clobbers r0.
 * ------------------------------------------------------------------------- */
	.global mmu_on
mmu_on:
	ldr r0, =STUB_TTB               @ low bits 0: NC, non-shareable walks (table is in NC MiB 0)
	mcr p15, 0, r0, c2, c0, 0       @ TTBR0
	mov r0, #0
	mcr p15, 0, r0, c2, c0, 2       @ TTBCR = 0: short descriptors, TTBR0 covers 4 GiB
	ldr r0, =0x55555555
	mcr p15, 0, r0, c3, c0, 0       @ DACR: all domains client
	isb
	mov r0, #0
	mcr p15, 0, r0, c8, c7, 0       @ TLBIALL (entries from a payload table must go)
	mcr p15, 0, r0, c7, c5, 0       @ ICIALLU
	mcr p15, 0, r0, c7, c5, 6       @ BPIALL
	dsb sy
	isb
	mrc p15, 0, r0, c1, c0, 0
	bic r0, r0, #SCTLR_A
	bic r0, r0, #(SCTLR_WXN | SCTLR_UWXN)
	bic r0, r0, #SCTLR_EE
	bic r0, r0, #(SCTLR_TRE | SCTLR_AFE)
	orr r0, r0, #(SCTLR_M | SCTLR_C)
	orr r0, r0, #(SCTLR_Z | SCTLR_I)
	mcr p15, 0, r0, c1, c0, 0
	isb
	bx  lr

/* ---------------------------------------------------------------------------
 * cache_sync_all -- clean+invalidate the D-cache by set/way over every level
 * CLIDR reports up to LoC (DCCISW, never DCISW: probe review F1), then
 * ICIALLU + BPIALL, DSB, ISB. Probe start.S step 4, unchanged loop body.
 * Touches this core's L1 and the shared L2 only -- callers serialise the two
 * cores (see IMPLEMENTATION-NOTES.md). AAPCS: saves r4-r11.
 * ------------------------------------------------------------------------- */
	.global cache_sync_all
cache_sync_all:
	push {r4-r11, lr}
	mrc p15, 1, r0, c0, c0, 1       @ CLIDR
	ands r3, r0, #0x7000000
	mov r3, r3, lsr #23             @ 2 * LoC
	beq 9f
	mov r10, #0                     @ 2 * level
5:	add r2, r10, r10, lsr #1        @ 3 * level
	mov r1, r0, lsr r2
	and r1, r1, #7                  @ Ctype
	cmp r1, #2
	blt 8f                          @ no data/unified cache here
	mcr p15, 2, r10, c0, c0, 0      @ CSSELR
	isb
	mrc p15, 1, r1, c0, c0, 0       @ CCSIDR
	and r2, r1, #7
	add r2, r2, #4                  @ set shift
	ldr r4, =0x3FF
	ands r4, r4, r1, lsr #3         @ max way
	clz r5, r4                      @ way shift
	ldr r7, =0x7FFF
	ands r7, r7, r1, lsr #13        @ max set
6:	mov r9, r4
7:	orr r11, r10, r9, lsl r5
	orr r11, r11, r7, lsl r2
	mcr p15, 0, r11, c7, c14, 2     @ DCCISW
	subs r9, r9, #1
	bge 7b
	subs r7, r7, #1
	bge 6b
8:	add r10, r10, #2
	cmp r3, r10
	bgt 5b
9:	dsb sy
	mov r0, #0
	mcr p15, 0, r0, c7, c5, 0       @ ICIALLU
	mcr p15, 0, r0, c7, c5, 6       @ BPIALL
	dsb sy
	isb
	pop {r4-r11, pc}

/* ---------------------------------------------------------------------------
 * payload_jump(entry, core) -- noreturn. Resets SP to this core's stub stack
 * top, sets the contract registers, FPSCR = 0, and branches. The C frame
 * that called us is abandoned on purpose.
 * ------------------------------------------------------------------------- */
	.global payload_jump
payload_jump:
	mov r2, r0
	mov r0, r1                      @ r0 = core id
	cmp r0, #0
	ldreq sp, =STUB_STACK0_TOP
	ldreq lr, =STUB_REENTER_CORE0
	ldrne sp, =STUB_STACK1_TOP
	ldrne lr, =STUB_REENTER_CORE1
	ldr r1, =STUB_MBOX_ADDR           @ r1 = &mbox
	mov r3, #0
	vmsr fpscr, r3
	dsb sy
	isb
	bx  r2

/* psci_cpu_on(target_mpidr, entry) -> PSCI return code (0 = SUCCESS).
 * SMC32 CPU_ON: r0=0x84000003, r1 target (0x00000001 -- masked, never the
 * raw MPIDR with bit31), r2 entry, r3 context id. */
	.global psci_cpu_on
psci_cpu_on:
	push {r4-r7, lr}                @ SMCCC 1.0 preserves r4+ but be conservative
	mov r2, r1
	mov r1, r0
	ldr r0, =0x84000003
	mov r3, #0
	smc #0
	pop {r4-r7, pc}

	.ltorg
