/* a32/common/stub_abi.h -- the A32 resident stub's fixed ABI.
 *
 * Shared by the stub (a32/stub), every payload (a32/payload-*, a32/renderer)
 * and the host tools (mkpayload.py / decode.py mirror these numbers). Plain
 * #defines only, so start.S files can include it too. The mailbox layout
 * itself is src/ipc/tr_mbox.h (authoritative); the MBOX_OFF_* below are the
 * few offsets assembly needs, checked against offsetof() in stub.c.
 *
 * ---------------------------------------------------------------------------
 * PAYLOAD CONTRACT (what a payload launched by ctrl_cmd=LAUNCH may assume and
 * must do). See a32/stub/IMPLEMENTATION-NOTES.md for the reasoning.
 *
 * Load: flat image at ctrl_entry, STUB_PAYLOAD_BASE <= ctrl_entry and
 *   ctrl_entry + ctrl_len <= STUB_PAYLOAD_LIMIT, ctrl_entry 4-byte aligned.
 *   ctrl_crc = zlib CRC32 of exactly those ctrl_len bytes. The CRC'd bytes
 *   must be immutable at run time (HALT then LAUNCH re-checks them without a
 *   reload): keep mutable state in .bss beyond ctrl_len, or re-copy .data
 *   from a pristine load image.
 *
 * Entry (both cores jump to ctrl_entry):
 *   r0 = core id (0 or 1), r1 = TR_MBOX_ADDR, lr = this core's re-entry
 *   address (STUB_REENTER_CORE0/1), sp = the stub's stack top for this core
 *   (16 KiB, NC; fine for startup, switch to your own).
 *   ARM state, SVC, NS, A/I/F masked. MMU on with the STUB table
 *   (TTBR0 = STUB_TTB, TTBCR 0, DACR all-client); D/I caches, branch
 *   prediction, NEON on; FPSCR = 0; CNTKCTL event stream on (WFE wakes
 *   every ~0.33-0.66 ms); VBAR = stub vectors (a fault records fault_* and
 *   parks -- a payload may keep them).
 *   Stub table: 0x020-0x023 SRAM0 Normal NC XN; 0x024 SRAM1 MiB0 Normal NC
 *   exec; 0x025 SRAM1 MiB1 Normal WB-WA S=1 exec; 0x026 WB-WA S=1 XN;
 *   0x027 translation fault; 0x800 MRAM WB read-only XN; rest Device XN.
 *   Core 0 enters first; core 1 is released right after (stub_core1_state
 *   is already RUNNING). If core 1 never started, only core 0 enters.
 *
 * Return (HALT, or whenever the payload is done):
 *   Core 1 first: branch to STUB_REENTER_CORE1 (or `bx lr` with the entry lr).
 *   Core 0: branch to STUB_REENTER_CORE0 (or `bx lr`). Consume HALT by
 *   writing ctrl_cmd = 0 before returning (the stub also clears a leftover).
 *   At the branch: PL1 (any mode), ARM state (use `bx` with bit0 = 0 from
 *   Thumb), and 0x024xxxxx must be mapped VA == PA, executable, in whatever
 *   table is live (or MMU off).
 *
 * HARD RULE: never write [0x02380000, 0x02381000) (STUB_MHU0_WINDOW), the
 *   TF-A MHU0 payload window; a write can corrupt TF-A/SE messaging. No
 *   framebuffer contains it any more (TR_FB_B moved to SRAM1 0x02600000,
 *   tr_mbox.h); the old stub-table test payloads, pinned to the original FB B
 *   0x02200000, still skip it byte-exactly.
 *
 * Payload page tables: 0x024xxxxx MUST be Normal Non-cacheable, VA == PA,
 *   executable, in every table a payload installs, for the whole run. The
 *   mailbox, stub code/.bss, stub stacks and tables live there; the stub's
 *   fault path runs under the payload's table until it switches back, and
 *   the M55 / debug AP read that page uncached. A cacheable alias would
 *   leave stale or dirty lines the stub never maintains. Nothing else needs
 *   restoring -- the re-entry path itself sets SVC, SP, VBAR, TTBR0 back to
 *   STUB_TTB (+TLBIALL), SCTLR, NEON/FPSCR, CNTKCTL, cleans+invalidates the
 *   D-cache by set/way (all levels) and invalidates I-cache + BTB, then sets
 *   stub_state = PARKED (from RUNNING) / stub_core1_state = PARKED.
 *   Core 0's re-entry waits <= 1 s for core 1 to leave RUNNING before its
 *   set/way pass; on timeout it records STUB_FAULT_CORE1_TIMEOUT.
 *
 * Faults: any exception into the stub vectors switches back to the stub
 *   table, records the FIRST fault in fault_* with its core id (a two-core
 *   lock makes simultaneous faults not interleave; later ones do not
 *   overwrite it), sets
 *   stub_state = FAULT and ctrl_cmd = HALT (so the sibling core's payload
 *   returns), and parks via the re-entry path. FAULT is parked: LAUNCH is
 *   accepted from PARKED or FAULT; HALT while parked is only cleared, it
 *   does not turn FAULT back into PARKED. A successful LAUNCH clears fault_*.
 *   A core that faults AGAIN before reaching its park loop stops in a WFE
 *   loop in SRAM1 (no heartbeat); for core 1 stub_core1_state becomes OFF, so
 *   the next LAUNCH still runs (core 0 only) instead of failing CORE1_BUSY.
 * ---------------------------------------------------------------------------
 */
#ifndef TR_STUB_ABI_H
#define TR_STUB_ABI_H

/* Where things are (plan sec 4, SRAM1 MiB 0). */
#define STUB_MBOX_ADDR    0x02401000 /* == TR_MBOX_ADDR, asserted in stub.c */
#define STUB_MRAM_BASE    0x80020000 /* A32_APP: TF-A erets here */
#define STUB_TTB          0x02404000 /* stub L1 table, 16 KiB */
#define STUB_BASE         0x0240C000 /* stub runs from here after relocation (<= 64 KiB) */
#define STUB_LIMIT        0x0241C000
#define STUB_STACK0_TOP   0x02420000 /* 0x0241C000..: core 0 */
#define STUB_STACK1_TOP   0x02424000 /* 0x02420000..: core 1 */
#define STUB_PAYLOAD_BASE 0x02500000
#define STUB_PAYLOAD_LIMIT \
	0x02580000 /* 512 KiB image (ctrl_len) budget; a payload's .bss may run past it */
/* Pre-relocation fault park page in SRAM0 (unused tail above FB B, the
 * probe's old park address): 8 vectors + WFE loop at +0x20, record at +0x40
 * = {STUB_EARLY_MAGIC, core, code, lr}. Written only if a fault hits before
 * the stub runs from SRAM1. */
/* TF-A MHU0 payload window (inside the ORIGINAL FB B 0x02200000, rows
 * 1092-1094). HARD RULE: no A32 code (stub or payload) ever writes it; reads
 * only for diagnosis. == tr_mbox.h TR_MHU0_WINDOW_LO/HI. */
#define STUB_MHU0_WINDOW      0x02380000
#define STUB_MHU0_WINDOW_SIZE 0x1000
#define STUB_EARLY_PARK       0x023FE000
#define STUB_EARLY_MAGIC      0x544C4645 /* "EFLT" */

/* Fixed entry points inside the relocated stub (stub start.S, .vectors). */
#define STUB_REENTER_CORE0 0x0240C020
#define STUB_REENTER_CORE1 0x0240C024

/* Release header, image offsets (patched by `mkpayload.py release`). The
 * payload lives in MRAM at STUB_MRAM_BASE + payload_off, is copied to
 * STUB_PAYLOAD_BASE and self-LAUNCHed. payload_len == 0: dev build. */
#define STUB_HDR_PAYLOAD_OFF 0x28
#define STUB_HDR_PAYLOAD_LEN 0x2C
#define STUB_HDR_PAYLOAD_CRC 0x30
#define STUB_HDR_MARK        0x34
#define STUB_HDR_MARK_VALUE  0x42555453 /* "STUB" */
#define STUB_MRAM_MAPPED_END 0x80100000 /* only MRAM section 0x800 is mapped */

/* ctrl_cmd / stub_state / stub_core1_state values (tr_mbox.h comments). */
#define STUB_CMD_NONE      0u
#define STUB_CMD_LAUNCH    1u
#define STUB_CMD_HALT      2u
#define STUB_STATE_PARKED  0u
#define STUB_STATE_RUNNING 1u
#define STUB_STATE_FAULT   2u
#define STUB_CORE1_OFF     0u
#define STUB_CORE1_PARKED  1u
#define STUB_CORE1_RUNNING 2u

/* fault_code values. fault_lr carries the banked LR for 1-4, and the
 * detail value noted for the software faults. */
/* The payload's launch token (renderer core-1 gate): stub_heartbeat0, bumped by every
 * launch, never 0. The renderer's RENDER_GATE is a fixed word at 0x025FE000, outside
 * .bss (core 0's zeroing never touches it), so on a cold page it holds power-on
 * garbage or an earlier launch's token; a token of 0 (the self-launch never ran the
 * park loop that used to bump it) could meet a gate word that reads 0 and open core 1
 * before core 0 has written the token. */
#ifndef __ASSEMBLER__
static inline unsigned stub_next_token(unsigned hb)
{
	return hb + 1u == 0u ? 1u : hb + 1u;
}
#endif

#define STUB_FAULT_NONE          0u
#define STUB_FAULT_UNDEF         1u  /* fault PC = lr - 4 */
#define STUB_FAULT_PABORT        2u  /* fault PC = lr - 4; IFSR/IFAR */
#define STUB_FAULT_DABORT        3u  /* fault PC = lr - 8; DFSR/DFAR */
#define STUB_FAULT_UNEXPECTED    4u  /* SVC/IRQ/FIQ/reserved vector */
#define STUB_FAULT_BAD_CRC       5u  /* lr = computed CRC, dfar = ctrl_entry */
#define STUB_FAULT_BAD_RANGE     6u  /* lr = ctrl_len, dfar = ctrl_entry */
#define STUB_FAULT_CORE1_BUSY    7u  /* LAUNCH while core 1 still RUNNING */
#define STUB_FAULT_CORE1_TIMEOUT 8u  /* core 1 did not park within 1 s of core 0 */
#define STUB_FAULT_PSCI          9u  /* lr = CPU_ON return (signed), or 1 = never parked */
#define STUB_FAULT_BAD_HEADER    10u /* release header out of range; lr = payload_len */

/* Previous boot's fault record, in the stub-owned ctrl-block padding
 * (tr_mbox_t.pad4[0..3]). Copied from fault_* at stub init when the mailbox
 * magic was already valid (a warm A32 restart), else zeroed. */
#define MBOX_OFF_LAST_FAULT_CORE 0x1A0
#define MBOX_OFF_LAST_FAULT_CODE 0x1A4
#define MBOX_OFF_LAST_FAULT_LR   0x1A8
#define MBOX_OFF_LAST_FAULT_DFAR 0x1AC
/* LAUNCH timing stamps, CNTVCT low 32 bits, pad4[4..7] (tr_mbox.h
 * TR_STUB_T_*): release copy start / end (0 on a dev boot), launch() entry,
 * payload jump. Zeroed at stub init. */
#define MBOX_OFF_T_COPY0  0x1B0
#define MBOX_OFF_T_COPY1  0x1B4
#define MBOX_OFF_T_LAUNCH 0x1B8
#define MBOX_OFF_T_JUMP   0x1BC

/* tr_mbox_t offsets used from assembly (asserted in stub.c). */
#define MBOX_OFF_CTRL_CMD   0x180
#define MBOX_OFF_STUB_STATE 0x190
#define MBOX_OFF_FAULT_CORE 0x1C0
#define MBOX_OFF_FAULT_CODE 0x1C4
#define MBOX_OFF_FAULT_LR   0x1D8

#endif /* TR_STUB_ABI_H */
