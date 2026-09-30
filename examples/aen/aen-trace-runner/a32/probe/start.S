/* start.S -- Cortex-A32 reset entry, cache/MMU bring-up, fault handlers.
 *
 * Boot contract (docs/2026-09-22-a32-probe-spec.md):
 *   TF-A sp_min erets to 0x80020000 in ARM state, SVC mode, Non-secure,
 *   IRQ/FIQ/Abort masked, MMU/caches off, SCTLR unprogrammed. No header is
 *   parsed -- the first word AT 0x80020000 is executed directly, so that
 *   word must be a valid ARM instruction: it is the reset vector below.
 *
 * ARM ARM references below are to the ARMv7-A/ARMv8-A (AArch32) ARM
 * (Cortex-A32 implements the ARMv8-A architecture in AArch32-only form,
 * and is programmed identically to an ARMv7-A core for everything used
 * here -- CLIDR/CCSIDR/CSSELR set/way maintenance and the VMSAv7
 * short-descriptor translation tables are unchanged).
 */

	.syntax unified
	.arm
	.section .vectors, "ax"
	.global _vectors

/* ---------------------------------------------------------------------
 * Layout constants -- MUST stay in lock-step with results.h and decode.py.
 * ------------------------------------------------------------------- */
	.equ RESULTS_BASE,     0x023FF000
	.equ RESULTS_SIZE,     0x35C          /* sizeof(results_t), see results.h (stage 2 grew this
	                                        * from stage 1's 0x2C0 -- fields only ever APPENDED) */
	.equ RESULTS_MAGIC,    0xA32A0001
	.equ OFF_STAGE,        0x04
	.equ OFF_FAULT_CODE,   0x08
	.equ OFF_DFSR,         0x0C
	.equ OFF_DFAR,         0x10
	.equ OFF_IFSR,         0x14
	.equ OFF_IFAR,         0x18
	.equ OFF_FAULT_LR,     0x1C
	.equ OFF_CNTFRQ,       0x20
	.equ OFF_SCTLR,        0x24
	.equ OFF_CPACR,        0x28
	.equ OFF_FPEXC,        0x2C
	/* stage 2: only the offsets start.S itself touches; everything else new
	 * (dual_go/done, cdc_*, ldrex_*, coh_*) is C-only, written from main.c
	 * via results.h, start.S never needs those offsets. */
	.equ OFF_SECONDARY_MPIDR,   0x2D0
	.equ OFF_SECONDARY_READY,   0x2D4
	.equ OFF_SECONDARY_PARKED,  0x308
	.equ OFF_PROBE_RECOVERY_PC, 0x354  /* non-fatal-fault handshake, see probe_read32/pmu_try_read */
	.equ OFF_PROBE_FAULTED,     0x358

	.equ TTB_BASE,         0x023E0000     /* 16 KiB, 4096 x 1 MiB section entries */
	.equ TTB_SIZE,         0x4000
	.equ STACK_TOP,        0x023FF000     /* == RESULTS_BASE: grows down, never crosses it */
	.equ PARK_LOOP_SRAM,   0x023FE000     /* core0's SRAM park loop + heartbeat */
	.equ CORE1_PARK_ADDR,  0x02402000     /* core1's OWN park loop + heartbeat, in SRAM1
	                                        * (addendum item D: core1 no longer shares core0's
	                                        * SRAM0 park loop) */
	.equ CORE1_STACK_TOP,  0x023D0000     /* core1's stack: same SRAM0 0x023xxxxx section as
	                                        * core0's, well below TTB_BASE (0x023E0000) and far
	                                        * from core0's stack (grows down from 0x023FF000).
	                                        * Grows down into 0x023CFFFC.. -- headroom before the
	                                        * next thing that matters (the TF-A secure MHU0
	                                        * window at 0x02380000-0x02380FFF, never to be
	                                        * touched) is 0x023D0000-0x02381000 = ~316 KiB, not
	                                        * the whole way to the section base 0x02300000 */
	.equ VECTORS_BASE,     0x80020000

	.equ FAULT_UNDEF,      1
	.equ FAULT_PABORT,     2
	.equ FAULT_DABORT,     3

	/* Section-descriptor attribute words (short-descriptor VMSAv7 Section
	 * format, first-level entry, bits[1:0]=0b10 | PXN):
	 *   [31:20] section base   [19] NS  [18] 0(Section)  [17] nG  [16] S
	 *   [15] APX  [14:12] TEX  [11:10] AP  [9] SBZ  [8:5] Domain
	 *   [4] XN  [3] C  [2] B  [1] 1  [0] PXN
	 * All entries here use APX=0,AP=0b11 (full access, "AP=full access"
	 * per spec), Domain=0, NS=0, nG=0, PXN=0 -- only TEX/C/B/XN/S vary,
	 * exactly per the spec's memory-plan table. */
	.equ ATTR_NC,          0x1C12  /* TEX=001 C=0 B=0, AP=11, XN=1        : Normal NC, not executable */
	.equ ATTR_NC_X,        0x1C02  /* same, XN=0                          : Normal NC, EXECUTABLE (0x023xxxxx only) */
	.equ ATTR_WB,          0x1C1E  /* TEX=001 C=1 B=1, AP=11, XN=1        : Normal WB-WA, not executable */
	.equ ATTR_WB_X,        0x1C0E  /* same, XN=0                          : Normal WB-WA, EXECUTABLE (MRAM) */
	.equ ATTR_DEV,         0x10C16 /* TEX=000 C=0 B=1 S=1, AP=11, XN=1    : Shareable Device */
	.equ ATTR_WB_S1,       0x11C1E /* = ATTR_WB | S(bit16) : Normal WB-WA, Inner Shareable, not
	                                 * executable -- addendum item A: the stage-1 ATTR_WB (S=0)
	                                 * is fine for one core; two cores caching the SAME line need
	                                 * S=1 so the coherency fabric (TF-A sets CPUECTLR.SMPEN, per
	                                 * the plan doc) keeps their L1 copies snoop-coherent. Used
	                                 * ONLY for the new dual-core regions (0x025xxxxx, 0x026xxxxx)
	                                 * -- the stage-1 WB buffer at 0x02100000 stays on ATTR_WB
	                                 * (S=0) so its single-core numbers remain comparable. */

_vectors:
	b   reset_handler       /* 0x00 Reset -- this is the word TF-A jumps to */
	b   undef_handler       /* 0x04 Undefined Instruction */
	b   svc_handler         /* 0x08 Supervisor Call (unused: we never execute SVC) */
	b   prefetch_abort_handler /* 0x0C Prefetch Abort */
	b   data_abort_handler  /* 0x10 Data Abort */
	b   trap_handler        /* 0x14 Reserved (Hyp entry on some cores; unused here) */
	b   irq_handler         /* 0x18 IRQ (masked at entry, stays masked) */
	b   fiq_handler         /* 0x1C FIQ (masked at entry, stays masked) */

	.text
	.arm

/* =======================================================================
 * reset_handler -- spec "Startup" steps 1-9.
 * ===================================================================== */
	.global reset_handler
reset_handler:
	/* --- step 1a: VBAR = our vector table, and force SCTLR.V=0 so VBAR
	 * actually takes effect (V=1 would keep exceptions at the legacy
	 * 0xFFFF0000 high-vector alias regardless of VBAR; TF-A leaves SCTLR
	 * "unprogrammed" so we cannot assume V's reset value). Do this before
	 * anything that could fault. */
	ldr r0, =VECTORS_BASE
	mcr p15, 0, r0, c12, c0, 0      @ VBAR = r0
	mrc p15, 0, r0, c1, c0, 0       @ SCTLR ->r0
	bic r0, r0, #(1 << 13)          @ clear V
	mcr p15, 0, r0, c1, c0, 0       @ SCTLR <- r0
	isb                             @ context-synchronise before relying on VBAR/SCTLR.V

	/* --- step 2: SP = 0x023FF000, grows down; results block lives AT and
	 * above this address, so the stack (pre-decrement STR/PUSH) never
	 * touches it. */
	ldr sp, =STACK_TOP

	/* --- zero the whole results block first (SRAM contents at cold boot
	 * are not defined by anything we control), then stamp header. This
	 * also means a run that never touches a field (e.g. no fault) reads
	 * back as a clean 0, not stale SRAM garbage. */
	ldr r0, =RESULTS_BASE
	mov r1, #0
	ldr r2, =(RESULTS_BASE + RESULTS_SIZE)
1:	str r1, [r0], #4
	cmp r0, r2
	blt 1b

	/* --- step 3: results header: magic, stage = 1. */
	ldr r0, =RESULTS_BASE
	ldr r1, =RESULTS_MAGIC
	str r1, [r0]
	mov r1, #1
	str r1, [r0, #OFF_STAGE]

	/* --- Install the SRAM park loop NOW, before any operation that can
	 * fault (page-table build is pure stores/CP15 writes and shouldn't
	 * fault, but step 7's NEON enable explicitly CAN take an undef). All
	 * three fault handlers below jump to PARK_LOOP_SRAM (via goto_park);
	 * it must exist before the first instruction that can raise a
	 * synchronous fault. The spec lists this as step 8 (after MMU/NEON
	 * bring-up); moving it earlier is a deliberate deviation for that
	 * reason -- see IMPLEMENTATION-NOTES.md. 0x023xxxxx is identity-mapped
	 * (VA==PA) both with the MMU off (direct physical write, as here) and
	 * once the MMU is on (our own section table maps it 1:1), so writing
	 * it this early is safe either way.
	 *
	 * Stage 2: the park loop now also increments a heartbeat word each
	 * WFI cycle (addendum item D, "heartbeats keep advancing after the
	 * tests end" -- lets the bench tell "parked and idle" apart from
	 * "wedged" after e.g. an HE Flow-C reset). install_heartbeat_park()
	 * copies the assembled template below rather than hand-encoding
	 * instruction words the way stage 1's 2-instruction version did --
	 * safer once the loop is more than a couple of instructions. */
	ldr r0, =PARK_LOOP_SRAM
	bl install_heartbeat_park

	/* =====================================================================
	 * step 4: invalidate I-cache, branch predictor, TLB, and clean+
	 * invalidate the D-cache by set/way over every level reported by
	 * CLIDR, BEFORE turning the MMU or caches on. This is the standard
	 * ARMv7-A/ARMv8-A(AArch32) "cache maintenance by set/way at reset"
	 * sequence (ARM ARM cache-maintenance example code, CLIDR/CCSIDR/
	 * CSSELR/DCCISW), with one deliberate change from the textbook
	 * invalidate-only version: this is a WARM handoff, not a cold reset --
	 * TF-A (sp_min and whatever ran before it) may have executed with
	 * caches on and left live dirty lines belonging to secure-world state
	 * (e.g. its own RW data, used again on the next SMC such as PSCI
	 * CPU_ON) still sitting in the L1/L2 arrays, unwritten to memory. A
	 * plain invalidate-by-set/way (DCISW) silently drops those dirty
	 * lines with no writeback -- for our OWN not-yet-used regions that's
	 * harmless, but for memory TF-A still owns it's data corruption. Using
	 * clean+invalidate (DCCISW) instead writes any dirty line back before
	 * discarding it: for the lines that are genuinely dirty secure-world
	 * data, that preserves it; for everything else (our own regions,
	 * which have no valid content yet), the clean is a same-value no-op.
	 * Same cost, no destructive-if-warm assumption.
	 * =================================================================== */
	mcr p15, 0, r0, c7, c5, 0      @ ICIALLU: invalidate entire I-cache to PoU
	mov r0, #0
	mcr p15, 0, r0, c7, c5, 6      @ BPIALL: invalidate entire branch predictor array
	mcr p15, 0, r0, c8, c7, 0      @ TLBIALL: invalidate entire unified TLB (both stages n/a, NS EL1&0)

	/* D-cache clean+invalidate by set/way, all levels (ARM ARM cache-
	 * maintenance example code, transcribed with the original register
	 * names so it can be checked against the reference line by line; the
	 * only change from the reference is DCISW -> DCCISW, see above). */
	mrc p15, 1, r0, c0, c0, 1      @ CLIDR -> r0
	ands r3, r0, #0x7000000
	mov r3, r3, lsr #23            @ r3 = 2 * (Level of Coherency), naturally aligned
	beq dcache_done
	mov r10, #0                    @ r10 = 2 * cache level, starts at level 0
dcache_level_loop:
	add r2, r10, r10, lsr #1       @ r2 = 3 * (r10/2) = 3 * cache level
	mov r1, r0, lsr r2             @ shift CLIDR to put this level's Ctype field in bits[2:0]
	and r1, r1, #7                 @ r1 = Ctype for this level
	cmp r1, #2
	blt dcache_level_skip          @ Ctype < 2 -> no data/unified cache at this level
	mcr p15, 2, r10, c0, c0, 0     @ CSSELR = r10 (select this level, data/unified side)
	isb                            @ CSSELR write must be visible before reading CCSIDR
	mrc p15, 1, r1, c0, c0, 0      @ CCSIDR -> r1 (geometry of the now-selected cache)
	and r2, r1, #7                 @ r2 = LineSize field
	add r2, r2, #4                 @ r2 = log2(line size in words) + 2 = set-index shift
	ldr r4, =0x3FF
	ands r4, r4, r1, lsr #3        @ r4 = Associativity - 1 (max way number)
	clz r5, r4                     @ r5 = bit position of the way field in the DCISW operand
	ldr r7, =0x7FFF
	ands r7, r7, r1, lsr #13       @ r7 = NumSets - 1 (max set/index number)
dcache_way_loop:
	mov r9, r4                     @ r9 = working way counter, reloaded from max each set
dcache_set_loop:
	orr r11, r10, r9, lsl r5       @ r11 = Level | (Way << way-shift)
	orr r11, r11, r7, lsl r2       @ r11 |= (Set << set-shift)
	mcr p15, 0, r11, c7, c14, 2    @ DCCISW: clean AND invalidate this set/way (not DCISW/c7,c6,2 --
	                                @ see the step-4 comment above: a bare invalidate can drop dirty
	                                @ secure-world data still resident from before this handoff)
	subs r9, r9, #1
	bge dcache_set_loop
	subs r7, r7, #1
	bge dcache_way_loop
dcache_level_skip:
	add r10, r10, #2               @ next level
	cmp r3, r10
	bgt dcache_level_loop
dcache_done:
	dsb                             @ ARM ARM: DSB required after set/way maintenance so it
	                                 @ has completed before anything else observes the caches
	isb                             @ synchronise the pipeline before continuing

	/* =====================================================================
	 * step 5: build the short-descriptor section table at TTB_BASE.
	 * First fill all 4096 entries with the Shareable Device default
	 * (TEX=000 C=0 B=1 -- this is Shareable Device, a distinct VMSAv7
	 * memory type from Strongly-ordered which is TEX=000 C=0 B=0; the
	 * spec's memory-plan table says "Device" and gives exactly this
	 * TEX/C/B, so Shareable Device is what "everything else | Device"
	 * means here) per "everything else | Device", then override the
	 * entries the memory plan calls out by name.
	 * =================================================================== */
	ldr r1, =TTB_BASE
	ldr r2, =ATTR_DEV
	mov r0, #0                     @ r0 = running "index << 20" (== section base address)
	ldr r3, =(TTB_BASE + TTB_SIZE)
fill_loop:
	orr r4, r0, r2                 @ descriptor = base | Device attrs
	str r4, [r1], #4
	add r0, r0, #0x00100000        @ next 1 MiB index
	cmp r1, r3
	blt fill_loop

	/* 0x02000000-0x020FFFFF: test buffer NC */
	ldr r1, =(TTB_BASE + (0x020 * 4))
	ldr r4, =(0x02000000 | ATTR_NC)
	str r4, [r1]

	/* 0x02100000-0x021FFFFF: test buffer WB (write-back write-allocate) */
	ldr r1, =(TTB_BASE + (0x021 * 4))
	ldr r4, =(0x02100000 | ATTR_WB)
	str r4, [r1]

	/* 0x02200000-0x022FFFFF: copy destination NC */
	ldr r1, =(TTB_BASE + (0x022 * 4))
	ldr r4, =(0x02200000 | ATTR_NC)
	str r4, [r1]

	/* 0x02300000-0x023FFFFF: page table / stack / results / park loop.
	 * Normal NC per the memory plan, but XN=0 -- this is the one SRAM
	 * section that must stay executable so the park loop at 0x023FE000
	 * can run (step 8's requirement). Never touches 0x02380000-
	 * 0x02380FFF itself (TF-A secure MHU0 payload window) -- that range
	 * gets the SAME section attributes as the rest of 0x023xxxxx (it is
	 * inside this 1 MiB section, mapped identically); we simply never
	 * issue any load/store/fetch at that address from this probe. */
	ldr r1, =(TTB_BASE + (0x023 * 4))
	ldr r4, =(0x02300000 | ATTR_NC_X)
	str r4, [r1]

	/* Stage 2 (SRAM1 0x02400000-0x027FFFFF, 4 MiB, per the addendum):
	 * 0x024xxxxx: Normal NC, EXECUTABLE -- core1's park loop/heartbeat
	 * (0x02402000) and the mailbox sentinel (0x02401000) live here.
	 * 0x025xxxxx, 0x026xxxxx: Normal WB-WA, Inner Shareable (ATTR_WB_S1)
	 * -- the dual-core fill/clean/read buffer, the LDREX/STREX counter,
	 * and the cross-L1 coherency buffer all live here.
	 * 0x027xxxxx: a genuine translation fault (descriptor 0), not just
	 * "Device and we chose not to touch it" (review item 10: the fill
	 * loop's default is Device/RW -- readable/writable, just never
	 * referenced by our own code; that's "we didn't", not "it can't".
	 * TF-A's own RW data lives at 0x027DE000-0x027ED000 in this section --
	 * a real fault-on-access is a much stronger guarantee against a future
	 * bug reaching in here than "nothing currently does"). */
	ldr r1, =(TTB_BASE + (0x024 * 4))
	ldr r4, =(0x02400000 | ATTR_NC_X)
	str r4, [r1]
	ldr r1, =(TTB_BASE + (0x025 * 4))
	ldr r4, =(0x02500000 | ATTR_WB_S1)
	str r4, [r1]
	ldr r1, =(TTB_BASE + (0x026 * 4))
	ldr r4, =(0x02600000 | ATTR_WB_S1)
	str r4, [r1]
	ldr r1, =(TTB_BASE + (0x027 * 4))
	mov r4, #0                      @ descriptor 0 = translation fault on any access
	str r4, [r1]

	/* 0x80000000-0x800FFFFF: MRAM, code+rodata, WB cacheable, executable
	 * (this is where we are running from). Only the ONE 1 MiB section the
	 * image actually occupies (it's ~2 KiB, nowhere near the 1 MiB
	 * boundary) -- least privilege: the other five MRAM megabytes the
	 * spec's memory plan allows for (up to 0x805FFFFF) stay at the
	 * Device/XN default from the fill loop above, so a wild pointer bug
	 * can't silently execute or cache-allocate against MRAM we never
	 * intended to touch. */
	ldr r1, =(TTB_BASE + (0x800 * 4))
	ldr r4, =(0x80000000 | ATTR_WB_X)
	str r4, [r1]

	/* TTBR0 = table base. The low 14 bits (walk-cacheability/shareability
	 * attributes) are all 0 because TTB_BASE is 16 KiB-aligned by
	 * construction: that means Non-cacheable, non-shareable walks, which
	 * matches the section attribute we just gave this table's own memory
	 * (0x023xxxxx is Normal NC) -- so there is no coherency gap between
	 * "what the walk hardware caches" and "what this NC region actually
	 * holds" (we also never modify the table again after this point). */
	ldr r0, =TTB_BASE
	mcr p15, 0, r0, c2, c0, 0      @ TTBR0 = r0

	mov r0, #0
	mcr p15, 0, r0, c2, c0, 2      @ TTBCR = 0: short-descriptor format, TTBR0 covers 4 GiB

	ldr r0, =0x55555555
	mcr p15, 0, r0, c3, c0, 0      @ DACR: every domain = Client (permissions come from AP bits)

	dsb                             @ all table stores + CP15 writes visible before MMU enable
	isb

	/* --- step 6: enable MMU (M), D-cache (C), I-cache (I), branch
	 * prediction (Z). Read-modify-write so we don't disturb bits TF-A may
	 * already have set/cleared for reasons outside this spec -- EXCEPT the
	 * handful below that our own page-table encoding has a hard dependency
	 * on, which we force to a known state rather than trust "unprogrammed":
	 *   A    (bit 1)  alignment-fault checking -- off, we don't want a
	 *                 stray unaligned access faulting instead of just working
	 *   WXN  (bit 19) Writable-implies-XN -- off, or it could silently XN
	 *                 our one deliberately-executable writable-looking
	 *                 0x023xxxxx section (the park loop lives there)
	 *   UWXN (bit 20) unprivileged-write-implies-XN at PL1 -- off, same
	 *                 family of hazard as WXN, we run everything at PL1
	 *   EE   (bit 25) exception endianness -- off (little-endian), matches
	 *                 every literal/struct in this image
	 *   TRE  (bit 28) TEX-remap enable -- MUST be off: every TEX/C/B value
	 *                 in our section table was chosen from the "TRE=0"
	 *                 default memory-type table (ARM ARM); if this were
	 *                 left on, the same bit patterns would mean different
	 *                 memory types and every attribute in the page table
	 *                 build above would be silently wrong
	 *   AFE  (bit 29) Access Flag Enable -- MUST be off: it changes what
	 *                 the AP[2:0]/APX field means, and our AP=0b11 "full
	 *                 access" encoding assumes the AFE=0 (3-bit AP) model
	 */
	mrc p15, 0, r0, c1, c0, 0      @ SCTLR -> r0
	bic r0, r0, #(1 << 1)           @ A
	bic r0, r0, #(1 << 19)          @ WXN
	bic r0, r0, #(1 << 20)          @ UWXN
	bic r0, r0, #(1 << 25)          @ EE
	bic r0, r0, #(1 << 28)          @ TRE
	bic r0, r0, #(1 << 29)          @ AFE
	orr r0, r0, #(1 << 0)          @ M
	orr r0, r0, #(1 << 2)          @ C
	orr r0, r0, #(1 << 11)         @ Z
	orr r0, r0, #(1 << 12)         @ I
	mcr p15, 0, r0, c1, c0, 0      @ SCTLR <- r0 (MMU + caches now live)
	isb                             @ ARM ARM: ISB required after enabling MMU/caches so that
	                                 @ every instruction after this point is fetched/decoded
	                                 @ under the new translation + cache state

	/* Stash the post-enable SCTLR for the results block now, while it's
	 * still hot in r0. */
	ldr r1, =RESULTS_BASE
	str r0, [r1, #OFF_SCTLR]

	/* CNTFRQ readback (spec step, "CNTFRQ = 100 MHz ... CNTVCT"). */
	mrc p15, 0, r0, c14, c0, 0     @ CNTFRQ -> r0
	str r0, [r1, #OFF_CNTFRQ]

	/* Enable the generic timer event stream (CNTKCTL.EVNTEN + EVNTI=15):
	 * review finding, BLOCKER. Every `wfe` in this image (barrier waits,
	 * the park loop) is relying on being woken periodically to re-check
	 * its condition -- but with CPSR.{A,I,F} masked for the whole image
	 * and TF-A never programming CNTKCTL for us, there is NO event source
	 * at all without this: a `wfe` with no pending event and no event
	 * stream sleeps until an explicit `sev` from another core, which
	 * means EVERY timeout-bounded wait in main.c was dead code -- it can
	 * only ever return via the flag actually being set, never via its own
	 * timeout check (that check only runs after `wfe` returns, and
	 * nothing was making it return). EVNTI=15 -> a stream event roughly
	 * every 2^15 CNTVCT ticks, ~655 us @ 100 MHz -- frequent enough that a
	 * 1 s timeout still fires promptly, infrequent enough not to matter
	 * for power/contention. */
	mov r0, #((15 << 4) | (1 << 2))  @ EVNTI=15, EVNTEN=1
	mcr p15, 0, r0, c14, c1, 0       @ CNTKCTL <- r0

	/* --- step 7: NEON/VFP enable. CPACR CP10/CP11 = full access, ISB,
	 * then FPEXC.EN. If Non-secure access to FPEXC is not granted, the
	 * `vmsr` below traps as an Undefined Instruction and undef_handler
	 * records fault_code=1 with the real LR -- we do not probe/guess NEON
	 * availability any other way. */
	mrc p15, 0, r0, c1, c0, 2      @ CPACR -> r0
	orr r0, r0, #(0xF << 20)       @ CP10 and CP11: full access at PL0 and PL1
	mcr p15, 0, r0, c1, c0, 2      @ CPACR <- r0
	isb                             @ CPACR write must take effect before the CP10/11 access below
	str r0, [r1, #OFF_CPACR]

	ldr r0, =0x40000000            @ FPEXC.EN
	vmsr fpexc, r0                 @ <-- can UNDEF here; see undef_handler
	str r0, [r1, #OFF_FPEXC]       @ only reached if the vmsr above didn't trap

	/* --- step 8 (remainder): the SRAM park loop copy already happened
	 * above, ahead of anything fault-capable. Nothing left to do here. */

	/* --- step 9: run the tests, then park. */
	bl main
	ldr r0, =RESULTS_BASE
	ldr r1, =0xD0E
	str r1, [r0, #OFF_STAGE]
	b goto_park

/* =======================================================================
 * Fault handlers. Each records fault_code + DFSR/DFAR/IFSR/IFAR + LR into
 * the results block (spec: "write a fault code + DFAR/DFSR/IFAR/IFSR and
 * LR into the results block, then jump to the SRAM park loop"), then never
 * returns. DFSR/DFAR/IFSR/IFAR are captured unconditionally for all three
 * fault types (cheap, and matches the results block's single fixed set of
 * slots -- an undef's DFSR/DFAR/IFSR/IFAR values are simply not meaningful
 * and a reader keys off fault_code first).
 * ===================================================================== */
undef_handler:
	mov r2, #FAULT_UNDEF
	b   record_fault
prefetch_abort_handler:
	mov r2, #FAULT_PABORT
	b   record_fault
data_abort_handler:
	mov r2, #FAULT_DABORT
	b   record_fault

record_fault:
	ldr r0, =RESULTS_BASE
	ldr r3, [r0, #OFF_PROBE_RECOVERY_PC]   @ stage 2: is this fault EXPECTED and recoverable
	cmp r3, #0                              @ (probe_read32/pmu_try_read arm this before a
	beq .Lfatal_fault                       @ risky access), or a genuine fatal fault?
	/* Review item 8 (minor): probe_recovery_pc isn't per-core -- only
	 * core0 ever arms it (PMU test and CDC probe both run on core0), but
	 * nothing previously stopped core1 taking the recoverable path if it
	 * happened to fault while core0's flag was armed (a narrow window,
	 * but real: core0's dual-core-round work overlaps in time with
	 * core1's). Guard explicitly on MPIDR rather than relying on that
	 * window never being hit. */
	mrc p15, 0, r1, c0, c0, 5      @ MPIDR -> r1
	and r1, r1, #0xFF               @ Aff0
	cmp r1, #0
	beq recoverable_fault           @ core0 faulting with it armed: the expected/non-fatal case
.Lfatal_fault:
	str r2, [r0, #OFF_FAULT_CODE]
	mrc p15, 0, r1, c5, c0, 0      @ DFSR
	str r1, [r0, #OFF_DFSR]
	mrc p15, 0, r1, c6, c0, 0      @ DFAR
	str r1, [r0, #OFF_DFAR]
	mrc p15, 0, r1, c5, c0, 1      @ IFSR
	str r1, [r0, #OFF_IFSR]
	mrc p15, 0, r1, c6, c0, 2      @ IFAR
	str r1, [r0, #OFF_IFAR]
	str lr, [r0, #OFF_FAULT_LR]    @ banked LR of whichever mode we trapped into
	b   goto_park

	/* Stage 2: recoverable-fault path. Never touches fault_code/DFSR/etc
	 * (those stay 0, meaning "no FATAL fault" -- still true). Instead of
	 * parking, this performs a proper exception return: overwrite the
	 * banked LR with the caller's chosen resume address, then `movs pc,
	 * lr` restores CPSR from SPSR (switching back to the SVC mode/state
	 * main() was running in) AND branches to that resume address in one
	 * instruction -- a plain `ldr pc,=...` would leave the core stuck in
	 * Abort/Undef mode with an uninitialised banked SP, corrupting the
	 * very next push/pop in the C code we're "returning" to. */
recoverable_fault:
	mov r1, #1
	str r1, [r0, #OFF_PROBE_FAULTED]
	mov r1, #0
	str r1, [r0, #OFF_PROBE_RECOVERY_PC]   @ disarm -- a LATER, unrelated fault must be fatal
	mov lr, r3
	movs pc, lr

/* svc/irq/fiq are not expected to ever fire (we never issue svc, and
 * IRQ/FIQ stay masked from entry onward per the boot contract); if one
 * somehow does, park rather than run off into the weeds. */
svc_handler:
irq_handler:
fiq_handler:
trap_handler:
	b goto_park

goto_park:
	/* Every caller (record_fault, core0's step-9 return path, and now
	 * core1's end-of-work path) lands here before jumping to SRAM; a
	 * single dsb at the funnel point covers all of them without repeating
	 * it at each call site. Drains any store still in flight -- the
	 * results-block writes each caller just did, and any NC/WB-S1 buffer
	 * writes from the tests -- so everything is actually in memory (and
	 * visible to an external debugger reading over SWD) before the core
	 * goes idle in the park loop.
	 *
	 * Stage 2: this is now SHARED between both cores (a fault on either
	 * core routes here through the same vector table), and the two cores
	 * park in DIFFERENT places (addendum item D: core1 has its own SRAM1
	 * park loop, not core0's SRAM0 one) -- so dispatch on MPIDR.Aff0,
	 * which is banked per-PE and always reflects whichever core is
	 * actually executing this instruction right now. */
	dsb sy
	sev                              @ review finding, BLOCKER, part 2: wake the OTHER core if
	                                  @ it's wfe-waiting on a barrier flag we're never going to
	                                  @ set (we're about to park) -- otherwise a core parking
	                                  @ after a timeout leaves the other core's timeout as its
	                                  @ only way out, and that timeout needs its own event
	                                  @ source anyway (see the CNTKCTL comment in reset_handler),
	                                  @ this is just an extra prompt wake, cheap and harmless
	mrc p15, 0, r0, c0, c0, 5      @ MPIDR -> r0
	and r0, r0, #0xFF               @ Aff0
	cmp r0, #0
	bne .Lpark_core1

	/* core0: relocate VBAR into this park page's own safe vector table
	 * (install_heartbeat_park wrote it at +0x40) before the final jump --
	 * see the install_heartbeat_park comment for why. */
	ldr r1, =(PARK_LOOP_SRAM + 0x40)
	mcr p15, 0, r1, c12, c0, 0
	isb
	ldr pc, =PARK_LOOP_SRAM         @ MRAM->SRAM is ~2 GiB away: needs an absolute jump, not `b`
.Lpark_core1:
	ldr r1, =(CORE1_PARK_ADDR + 0x40)
	mcr p15, 0, r1, c12, c0, 0
	isb
	ldr pc, =CORE1_PARK_ADDR

/* =======================================================================
 * install_heartbeat_park -- copy the assembled wfe+heartbeat-increment
 * loop template to r0 (invalidate I-cache for the freshly written
 * instructions), THEN write an 8-word "b ." safe vector table at
 * r0+0x40 (32-byte aligned -- VBAR requires that). Used for BOTH cores'
 * park pages (core0: PARK_LOOP_SRAM; core1: CORE1_PARK_ADDR) -- see the
 * reset_handler and secondary_entry call sites. r0 = destination;
 * clobbers r0-r3.
 * ===================================================================== */
	.global install_heartbeat_park
install_heartbeat_park:
	push {r4, lr}
	mov r4, r0
	ldr r0, =heartbeat_park_template
	mov r1, r4
	mov r2, #HEARTBEAT_PARK_TEMPLATE_WORDS
1:	ldr r3, [r0], #4
	str r3, [r1], #4
	subs r2, r2, #1
	bne 1b

	/* Review item 7 (minor, but a real gap): VBAR was left pointing at
	 * MRAM (0x80020000) forever, including while parked -- so any
	 * exception after park (there shouldn't be one, but "shouldn't" isn't
	 * "can't": an async abort, or a debugger-injected exception) would
	 * fetch its vector from MRAM, which is exactly the memory a Flow D
	 * write might be tearing while the core sits here. Give each park
	 * page its own tiny safe vector table -- 8x "b ." (branch to self,
	 * valid at every one of the 8 slots independently) at destination+
	 * 0x40 -- and goto_park points VBAR at it before the final jump. */
	add r1, r4, #0x40
	ldr r3, =0xEAFFFFFE      @ b .  (branch-to-self: offset (0-8)>>2 = -2 = 0xFFFFFE)
	mov r2, #8
2:	str r3, [r1], #4
	subs r2, r2, #1
	bne 2b

	dsb
	mov r0, #0
	mcr p15, 0, r0, c7, c5, 0       @ ICIALLU: invalidate I-cache (freshly-written instructions,
	                                 @ same self-modifying-code rule as stage 1's version of this)
	isb
	pop {r4, pc}

	.align 2
heartbeat_park_template:
1:	wfe   @ NOT wfi (review finding, MAJOR): wfi only wakes on an unmasked interrupt, and every
	      @ interrupt source is masked for the whole life of this image (CPSR.I/F set at TF-A
	      @ handoff, never cleared) -- a wfi park loop would NEVER wake to increment its own
	      @ heartbeat, making both stage 5's "heartbeats keep advancing" requirement and any
	      @ bench liveness check false. wfe wakes on the CNTKCTL event stream (enabled above,
	      @ reset_handler and secondary_entry) as well as on an explicit sev from the other core
	      @ or goto_park, so the loop actually runs.
	adr r0, heartbeat_park_template_slot @ PC-relative: correctly finds ITS OWN copy's heartbeat
	                                       @ word regardless of where this template gets copied to
	                                       @ (core0's PARK_LOOP_SRAM or core1's CORE1_PARK_ADDR) --
	                                       @ that is exactly why this is `adr`, not an absolute
	                                       @ `ldr r0,=0x023FE008`-style constant
	ldr r1, [r0]
	add r1, r1, #1
	str r1, [r0]
	b 1b
heartbeat_park_template_slot:
	.word 0                          @ heartbeat counter -- starts at 0 in the template, and in
	                                   @ every copy, since it's copied byte-for-byte from here
heartbeat_park_template_end:
	.equ HEARTBEAT_PARK_TEMPLATE_WORDS, (heartbeat_park_template_end - heartbeat_park_template) / 4

/* =======================================================================
 * probe_read32 -- read *addr, catching a fault instead of parking.
 * r0 = address. Returns (64-bit AAPCS: r0=low, r1=high): r0 = value read
 * (0 if it faulted), r1 = 1 if it faulted else 0. See the OFF_PROBE_*
 * comment in results.h -- single-core-at-a-time by construction (only
 * core0 ever calls this).
 * ===================================================================== */
	.global probe_read32
probe_read32:
	push {r4, lr}
	mov r4, r0
	ldr r2, =RESULTS_BASE
	mov r3, #0
	str r3, [r2, #OFF_PROBE_FAULTED]
	ldr r3, =.Lprobe_read32_resume
	str r3, [r2, #OFF_PROBE_RECOVERY_PC]   @ arm: a fault on the next load resumes here, not fatal
	ldr r0, [r4]                            @ <-- the risky access; ARM data aborts do NOT write
	                                         @     back the destination register, so on a fault r0
	                                         @     is left holding whatever it had before this line
.Lprobe_read32_resume:
	ldr r2, =RESULTS_BASE
	mov r3, #0
	str r3, [r2, #OFF_PROBE_RECOVERY_PC]   @ disarm unconditionally (success or fault)
	ldr r1, [r2, #OFF_PROBE_FAULTED]
	cmp r1, #0
	movne r0, #0                            @ fault path: r0's contents are unspecified, force 0
	pop {r4, pc}

/* =======================================================================
 * pmu_try_read -- enable PMCR.E + PMCNTENSET.C and read PMCCNTR, catching
 * an undef (NS access to the PMU can be blocked by SDCR/MDCR) instead of
 * treating it as fatal. Returns (r0=low,r1=high): r0 = PMCCNTR value (0 if
 * any step faulted), r1 = 1 if available else 0.
 * ===================================================================== */
	.global pmu_try_read
pmu_try_read:
	push {lr}
	ldr r2, =RESULTS_BASE
	mov r3, #0
	str r3, [r2, #OFF_PROBE_FAULTED]
	ldr r3, =.Lpmu_resume
	str r3, [r2, #OFF_PROBE_RECOVERY_PC]

	mrc p15, 0, r0, c9, c12, 0      @ PMCR -> r0
	orr r0, r0, #1                   @ E: enable all counters
	mcr p15, 0, r0, c9, c12, 0      @ PMCR <- r0                       (may UNDEF)
	mov r0, #0
	mcr p15, 0, r0, c14, c15, 7     @ PMCCFILTR = 0: count cycles unconditionally (no P/U/NS/S
	                                 @ mode filtering) -- explicit rather than trusting reset
	                                 @ state, review item 9 (minor; harmless either way here
	                                 @ since TF-A leaves SDCR.SPME=0/PMCR.D=0, no AArch32 monitor
	                                 @ trap on this path, but not worth relying on that)
	ldr r0, =0x80000000
	mcr p15, 0, r0, c9, c12, 1      @ PMCNTENSET.C: enable the cycle counter (may UNDEF)
	mrc p15, 0, r0, c9, c13, 0      @ PMCCNTR -> r0                     (may UNDEF)

.Lpmu_resume:
	ldr r2, =RESULTS_BASE
	mov r3, #0
	str r3, [r2, #OFF_PROBE_RECOVERY_PC]
	ldr r1, [r2, #OFF_PROBE_FAULTED]
	cmp r1, #0
	moveq r1, #1                     @ no fault -> available=1, r0 already holds PMCCNTR
	movne r0, #0                     @ fault -> value forced 0
	movne r1, #0                     @ fault -> available=0
	pop {pc}

/* =======================================================================
 * psci_cpu_on_secondary -- PSCI CPU_ON (SMC32 id 0x84000003) for core 1.
 * r1 (target MPIDR) is DERIVED from core0's own MPIDR (task's explicit
 * instruction), not hardcoded: keep Aff1/Aff2, clear everything else, set
 * Aff0=1 -- the standard "next core in the same cluster" convention for a
 * 2-core A32 pair. If that assumption is wrong, PSCI itself reports it
 * (INVALID_PARAMETERS or similar) via the return code we hand back -- not
 * a silent hang.
 * Returns r0 = SMC return code (0 = PSCI_E_SUCCESS).
 * ===================================================================== */
	.global psci_cpu_on_secondary
psci_cpu_on_secondary:
	push {lr}
	mrc p15, 0, r1, c0, c0, 5       @ MPIDR (core0's own) -> r1
	ldr r0, =0x00FFFF00              @ keep Aff1 (bits 15:8) + Aff2 (bits 23:16); review item 4
	and r1, r1, r0                   @ (MAJOR): a bare `bic Aff0` left bit31 (RES1) and bits
	                                  @ 30:24 (U/MT/reserved) copied straight from MPIDR into the
	                                  @ PSCI mpidr argument, which the PSCI spec requires to be 0
	                                  @ there. Alif's own ensemble_pwr_domain_on() uses this value
	                                  @ raw as an array index (secondary_cpu_flags[cpu-1]) and
	                                  @ only "worked" via 32-bit wraparound in that bl32 build --
	                                  @ not something to depend on.
	orr r1, r1, #1                   @ target = core 1 (Aff0=1, same cluster) -> 0x00000001
	ldr r0, =0x84000003              @ PSCI_CPU_ON (SMC32)
	ldr r2, =secondary_entry         @ entry point
	mov r3, #0                       @ context id, unused
	smc #0
	pop {pc}                          @ r0 = PSCI return code

/* =======================================================================
 * atomic_increment_n -- `*addr += 1`, `count` times, via LDREX/STREX.
 * r0 = address, r1 = count. The standard correct LDREX/STREX RMW retry
 * pattern: only the FAILED attempt (another observer's STREX to the same
 * exclusive reservation granule won the race) is retried, not the whole
 * count -- STREX failing must not double-increment. Used from BOTH cores
 * concurrently on the SAME address (addendum item B); correctness here
 * depends on the two cores sharing one global exclusive-monitor domain,
 * which is exactly what this test exists to prove.
 * ===================================================================== */
	.global atomic_increment_n
atomic_increment_n:
.Latomic_retry:
	ldrex r2, [r0]
	add r2, r2, #1
	strex r3, r2, [r0]
	cmp r3, #0
	bne .Latomic_retry                @ STREX lost the race -- retry this SAME increment
	subs r1, r1, #1
	bne .Latomic_retry
	bx lr

/* =======================================================================
 * secondary_entry -- core 1's PSCI CPU_ON entry point.
 *
 * PSCI CPU_ON's entry-state contract isn't spelled out by our own spec
 * (only core0's primary boot contract is, from TF-A source). This assumes
 * the standard PSCI/TF-A convention: ARM state, SVC, Non-secure, MMU/
 * caches off, IRQ/FIQ masked -- the same shape as core0's own primary
 * entry, which IS documented. If that assumption is wrong, the MMU-enable
 * sequence below simply faults like any other fault and is recorded
 * (fatal path, since PROBE_RECOVERY_PC is never armed here) -- not
 * silently wrong.
 * ===================================================================== */
	.global secondary_entry
secondary_entry:
	ldr r0, =VECTORS_BASE
	mcr p15, 0, r0, c12, c0, 0       @ VBAR: banked per-PE -- core0's own write never applied here
	mrc p15, 0, r0, c1, c0, 0
	bic r0, r0, #(1 << 13)            @ clear V, same reasoning as core0's reset_handler
	mcr p15, 0, r0, c1, c0, 0
	isb

	ldr sp, =CORE1_STACK_TOP           @ own stack -- see the .equ comment for why this can't
	                                     @ collide with core0's

	/* Install core1's OWN park loop/heartbeat in SRAM1 before anything
	 * fault-capable runs below (same "must exist before the first
	 * possible fault" reasoning as core0's reset_handler) -- SRAM1
	 * 0x024xxxxx is identity-mapped (VA==PA) whether or not core1's own
	 * MMU is on yet, exactly like core0's 0x023xxxxx. */
	ldr r0, =CORE1_PARK_ADDR
	bl install_heartbeat_park

	/* No D-CACHE SET/WAY invalidate here -- deliberate, not an omission,
	 * but NOT because "set/way is cluster-wide" (review item 12: that
	 * claim was wrong -- set/way maintenance is issued per-core and only
	 * ever touches the ISSUING core's own L1, plus whatever L2 it shares
	 * with others; it does not reach into another core's L1). The real
	 * reason core1 doesn't need it: core1's own L1 has never held
	 * anything until this instruction -- the core's own reset sequence
	 * invalidates its L1/TLB before it starts fetching at all, so there
	 * is nothing dirty or stale in it to clean. The shared L2 (if any)
	 * was already cleaned by core0's set/way sweep before core1 was ever
	 * powered on. From here on, TF-A's CPUECTLR.SMPEN (set before
	 * releasing either core, per the ARMv7-A/ARMv8-A multiprocessing-
	 * extensions contract) keeps the two L1s coherent via the snoop unit
	 * -- that's what makes "core0 cleaned it once, core1 trusts that" a
	 * sound argument instead of a race. (ARM's own multi-core bring-up
	 * guidance also flags set/way from a second core as actively unsafe
	 * once concurrent access from the first is possible -- another reason
	 * not to redo it here even if there WERE something to clean.)
	 *
	 * TLB/I-cache/branch-predictor ARE still invalidated (review item 11)
	 * -- cheap, and this core has never run this table through its own
	 * TLB or fetched through its own I-cache/BTB before. */
	mov r0, #0
	mcr p15, 0, r0, c7, c5, 0        @ ICIALLU
	mcr p15, 0, r0, c7, c5, 6        @ BPIALL
	mcr p15, 0, r0, c8, c7, 0        @ TLBIALL
	dsb
	isb

	/* Point at the SAME table core0 already built -- do not rebuild it. */
	ldr r0, =TTB_BASE
	mcr p15, 0, r0, c2, c0, 0        @ TTBR0
	mov r0, #0
	mcr p15, 0, r0, c2, c0, 2        @ TTBCR = 0
	ldr r0, =0x55555555
	mcr p15, 0, r0, c3, c0, 0        @ DACR
	dsb
	isb

	/* Same SCTLR bit-forcing as core0 (see the step-6 comment in
	 * reset_handler for why each of these matters, especially TRE/AFE). */
	mrc p15, 0, r0, c1, c0, 0
	bic r0, r0, #(1 << 1)
	bic r0, r0, #(1 << 19)
	bic r0, r0, #(1 << 20)
	bic r0, r0, #(1 << 25)
	bic r0, r0, #(1 << 28)
	bic r0, r0, #(1 << 29)
	orr r0, r0, #(1 << 0)
	orr r0, r0, #(1 << 2)
	orr r0, r0, #(1 << 11)
	orr r0, r0, #(1 << 12)
	mcr p15, 0, r0, c1, c0, 0
	isb

	mrc p15, 0, r0, c1, c0, 2        @ CPACR -> r0
	orr r0, r0, #(0xF << 20)
	mcr p15, 0, r0, c1, c0, 2        @ CPACR <- r0
	isb
	ldr r0, =0x40000000
	vmsr fpexc, r0                    @ <-- can UNDEF here too, exactly like core0's; same fatal
	                                    @     path (record_fault), since PROBE_RECOVERY_PC isn't
	                                    @     armed on this core

	/* CNTKCTL event stream, core1's own copy of the reset_handler fix
	 * (review item 1, BLOCKER) -- this register is per-PE, core0's write
	 * never applied here. Without it, every wfe in secondary_main's
	 * barrier waits has no event source but an explicit sev. */
	mov r0, #((15 << 4) | (1 << 2))  @ EVNTI=15, EVNTEN=1
	mcr p15, 0, r0, c14, c1, 0

	mrc p15, 0, r0, c0, c0, 5        @ MPIDR -> r0 (proof this is really core1's own code running)
	ldr r1, =RESULTS_BASE
	str r0, [r1, #OFF_SECONDARY_MPIDR]
	dsb sy
	mov r0, #1
	str r0, [r1, #OFF_SECONDARY_READY]
	dsb sy
	sev                                 @ wake core0 if it's WFE-waiting on secondary_ready

	bl secondary_main                   @ C: run the dual-core rounds (main.c)

	ldr r1, =RESULTS_BASE
	mov r0, #1
	str r0, [r1, #OFF_SECONDARY_PARKED]  @ tell core0/the bench core1 finished its rounds
	b goto_park                           @ MPIDR dispatch in goto_park sends this to CORE1_PARK_ADDR
