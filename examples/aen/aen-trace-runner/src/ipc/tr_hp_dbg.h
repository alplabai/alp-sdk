/* src/ipc/tr_hp_dbg.h -- the HP's bench-readable per-stage DWT timing +
 * status beacon, shared between hp_vision (the writer, hp_vision/src/main.c)
 * and the HE's HUD (the reader, src/platform/hud_l2.c) -- fix round 5:
 * previously hp_dbg_t was private to hp_vision/src/main.c, so the HUD had no
 * canonical layout to read it against and instead showed the P10 SOUND
 * ring's status word, which hp_vision never writes (it is not the sound
 * firmware) -- always "M55-HP --". Address: tr_memmap.h TR_MEM_HP_DBG.
 *
 * Word 0..3 match scripts/bench/aen/flash-jlink-hp.sh's generic beacon shape
 * (magic/CPUID/VTOR/heartbeat) -- that script accepts any SRAM0 address as
 * its beacon and just displays the first four words.
 */
#ifndef TR_HP_DBG_H
#define TR_HP_DBG_H

#include <stdbool.h>
#include <stdint.h>

#define TR_HP_DBG_MAGIC 0xA11FE000u

typedef struct {
	uint32_t magic, cpuid, vtor, heartbeat;
	uint32_t cpu_mhz;
	uint32_t frame_no;
	uint32_t capture_us, pre_us, infer_us, decode_us, thumb_us, ae_us;
	uint32_t loop_hz_x10; /* sustained 1 s rolling rate, whole pipeline */
	int32_t  status;      /* last alp_status_t != ALP_OK, 0 = none seen */
	/* SRAM1/CAM_POOL gate (fix round 2, hp_vision's tr_sram1_ready()):
	 * whether the gate has EVER passed this boot, and the heartbeat count
	 * it first passed at -- 0 means "never yet". */
	uint32_t sram1_ready_seen, sram1_ready_at_heartbeat;
	/* fix round 5: the HP's own busy/total cycle counters, CUMULATIVE
	 * since boot (uint64_t: at ~400 MHz a uint32_t would wrap every
	 * ~10.7 s, well inside a HUD sampling window) -- the SAME shape as
	 * src/hud/hud.h's tr_perf_raw_t.he_busy_cyc/he_all_cyc, so the HE
	 * computes a load % from the delta between two samples the identical
	 * way it already does for itself (src/hud/hud.c tr_perf_sample()),
	 * not by reconstructing an estimate from the per-stage *_us means
	 * above times loop_hz_x10 (that assumes a uniform cadence the real
	 * loop does not have -- capture-wait time is not "work"). busy_cyc
	 * accumulates DWT cycles spent in tr_movenet_input/invoke/decode +
	 * make_thumb + the AE step; total_cyc accumulates the whole loop
	 * period, capture-wait included. */
	uint64_t busy_cyc, total_cyc;
	/* fix round 5: the software AE's live state (src/vision/camera_ae.h),
	 * for the bench/HUD to see convergence without a live console. */
	uint32_t ae_exposure, ae_gain_idx;
	/* fix round 11 (silicon finding: "one beacon read per boot was torn,
	 * all timings 0" -- a reader landing between hp_vision's separate
	 * stores to the *_us timing block, or between its once-a-second
	 * loop_hz_x10 update, seeing an inconsistent/incomplete snapshot, the
	 * SAME class of gap tr_pslot.h's header note documents for tr_pslot_t
	 * and this file's own busy_cyc/total_cyc comment documents for a
	 * single 64-bit field -- but those two fields already had SOME
	 * protection (tr_hp_dbg_read_stable below); nothing protected the
	 * *_us block or loop_hz_x10 at all). APPENDED, not inserted: every
	 * field above keeps its existing offset (FLASH-RECIPE.md and any bench
	 * tooling that computed one by hand stays correct); only a reader that
	 * wants the *_us block or loop_hz_x10 needs to look at this. Textbook
	 * odd/even seqlock, tr_pslot_h's own protocol: the writer bumps seq
	 * odd before touching either group, even after; tr_hp_dbg_read() below
	 * is the matching reader. */
	uint32_t seq;
	/* fix round 12 finding A (silicon: exposure/gain writes appear to have
	 * ZERO effect on the captured frame -- ae_exposure above reads back at
	 * the new ceiling, gain climbs 11 -> 70, raw frame mean never moves).
	 * Raw register READBACK (not the values hp_vision THINKS it wrote --
	 * what the sensor says is actually sitting in each register right
	 * now), read right after every apply_ae() I2C write: 0x3500/01/02
	 * (exposure H/M/L), 0x3503 (AEC manual-mode bit -- reasserted every
	 * write in case something else resets it), 0x3509 (analogue gain),
	 * 0x380E/0F (VTS, re-read as a sensor-is-actually-answering canary,
	 * not just a first-boot ceiling derivation). ae_write_rc is the WORST
	 * (most negative) of every I2C call apply_ae() made this pass, 0 if
	 * all of them returned 0 -- the SAME "writes returning an error we
	 * ignore" possibility the finding named, now visible instead of
	 * silently discarded. */
	uint8_t  ae_reg_exp_h, ae_reg_exp_m, ae_reg_exp_l, ae_reg_mode;
	uint8_t  ae_reg_gain;
	uint16_t ae_reg_vts;
	int32_t  ae_write_rc;
} hp_dbg_t;

#include <stddef.h>
/* fix round 12 (review): no static assert pinned this layout before -- the
 * struct grew silently (round 5's own field list totals 0x58; adding seq
 * at 0x58 pushes the raw total to 0x5C, but the struct's overall alignment
 * is 8, uint64_t busy_cyc/total_cyc's own requirement, so sizeof() pads UP
 * to 0x60) and nothing caught tr_memmap.h's comment going stale (it still
 * said 0x58) or the fact that TR_MEM_CAM_VIEW started exactly 0x60 past
 * TR_MEM_HP_DBG, leaving zero headroom -- both fixed this round, this
 * assert is what keeps either from silently drifting again. */
_Static_assert(sizeof(hp_dbg_t) == 0x68,
               "hp_dbg_t size drifted (fix round 12 finding A added the AE "
               "register readback fields) -- update tr_memmap.h's TR_MEM_HP_DBG/"
               "TR_MEM_CAM_VIEW comments (headroom) and this assert together");
_Static_assert(offsetof(hp_dbg_t, busy_cyc) == 0x40 && offsetof(hp_dbg_t, ae_exposure) == 0x50 &&
                   offsetof(hp_dbg_t, seq) == 0x58 && offsetof(hp_dbg_t, ae_reg_exp_h) == 0x5C &&
                   offsetof(hp_dbg_t, ae_reg_vts) == 0x62 &&
                   offsetof(hp_dbg_t, ae_write_rc) == 0x64,
               "hp_dbg_t field offsets drifted -- see this file's own layout comment");

/* fix round 7 (silicon finding: hud.c computed a mathematically correct
 * ratio from these two fields -- host-verified against the exact reported
 * numbers, 787,127,318 / 790,822,175 rounds to 100%, not the 0% the HUD
 * showed -- so the gap is in HOW they get READ, not the arithmetic). Unlike
 * every other cross-core structure here (tr_pslot_t, tr_aring_t), busy_cyc/
 * total_cyc have no seqlock at all: a 64-bit value has no single-instruction
 * atomic load on this 32-bit core, so a read landing mid-update can tear
 * (read half the old value, half the new) -- and unlike a full struct, a
 * TORN 64-bit read of a field that INCREASES BY A FEW THOUSAND CYCLES A READ
 * can, worst case, momentarily read as a value SMALLER than the reader's own
 * previous sample (if the high word already advanced but the low word not
 * yet, or vice versa, depending on read order) -- the exact shape that
 * would make one specific window's delta compute near zero against an
 * otherwise-healthy series, without ever fully explaining a PERSISTENT 0%.
 * Given that residual uncertainty, this closes the actual protocol gap
 * rather than chasing the single reading further: each field is
 * individually MONOTONIC (only ever increases), so the standard lock-free
 * fix needs no writer-side protocol change at all -- read it twice and
 * accept only when both reads agree; a torn read cannot produce the exact
 * same wrong value twice in a row against a race window this short.
 * Function-pointer read callback (not a raw pointer) so this is
 * host-testable: tests/host/test_hp_dbg_stable_read.c supplies a mock that
 * returns a scripted, torn-looking sequence. */
static inline uint64_t tr_hp_dbg_read_stable(uint64_t (*read_once)(void *ctx), void *ctx)
{
	uint64_t a = read_once(ctx), b;

	do {
		b = a;
		a = read_once(ctx);
	} while (a != b);
	return a;
}

/* fix round 11: a proper odd/even seqlock read of the WHOLE struct (the
 * .seq field's own comment above) -- the writer's matching half is
 * tr_hp_dbg_write_seq_odd()/tr_hp_dbg_write_seq_even() below, wrapped
 * around whichever group of fields it is updating together (hp_vision/
 * src/main.c: the *_us timing block, and separately loop_hz_x10). A
 * reader that only cares about fields from ONE such group (e.g. render.c's
 * magic + loop_hz_x10) still needs THIS, not a raw field read: without it,
 * a read landing mid-update sees an odd seq that never gets rejected,
 * exactly the "torn" symptom the silicon finding reports. Same shape as
 * tr_pslot_read() (tr_pslot.h): reject on sight if seq is odd, copy the
 * whole struct between two barriers, reject if seq changed during the
 * copy. Struct copy from volatile, same idiom tr_pslot.c already uses. */
static inline bool tr_hp_dbg_read(const volatile hp_dbg_t *s, hp_dbg_t *out, void (*barrier)(void))
{
	uint32_t s0 = s->seq;

	if (s0 & 1u) {
		return false;
	}
	barrier();
	*out = *s;
	barrier();
	return s->seq == s0;
}

/* Writer's half, tr_hp_dbg_read()'s own comment: call _odd() before the
 * first store of a field group, _even() after the last -- every store in
 * between is then "torn-visible" to a concurrent reader. */
static inline void tr_hp_dbg_write_seq_odd(volatile hp_dbg_t *s, void (*barrier)(void))
{
	s->seq = s->seq + 1u;
	barrier();
}

static inline void tr_hp_dbg_write_seq_even(volatile hp_dbg_t *s, void (*barrier)(void))
{
	barrier();
	s->seq = s->seq + 1u;
}

#endif /* TR_HP_DBG_H */
