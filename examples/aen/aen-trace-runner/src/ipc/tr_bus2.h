/* src/ipc/tr_bus2.h -- the I2C2 + GPIO5 lease between the two M55 cores, for the combined HP image
 * (hp_vision with -DTR_HP_SOUND=ON: camera + NPU + game sound; docs/2026-09-23-sound.md, "Sound
 * with the vision HP").
 *
 * WHY A LEASE. Carrier bus 0 (SoC I2C2, 0x49012000) and the GPIO5 port (0x49005000) are shared:
 *   - the HE reads the +5V INA236 (0x4A, the HUD power line), the BMI323 (tilt) and, at boot, the
 *     display shield's TCAL9538 expander (0x73) on I2C2;
 *   - the HP's amp bring-up (sound/src/main.c) reads the identity EEPROM (0x50), resets and
 *     programs both TAS2563 (0x4D / 0x4E) on I2C2 and drives SD_N P5_2 / IRQZ P5_0 on GPIO5
 *     (read-modify-write of GPIO5 DR/DDR).
 * One DesignWare I2C2 serves both cores and its IRQ 134 reaches both NVICs, so an idle core with
 * the i2c_dw driver up still runs its ISR on the other core's transfer (silicon, hp_vision fix
 * round 6). So the bus has exactly one holder at a time, and only the holder's NVIC line is on.
 * Streaming is I2S3 only and needs no bus: the HP leases the bus for the bring-up (and for any
 * later runtime amp write) and gives it BACK to the HE, so the HUD power line keeps working.
 *
 * It is a TWO-WAY handoff, not alp,i2c-handover (zephyr/soc-bridge/alif/i2c_handover.h): that glue
 * is one-way (a release is taken once, never returned), blocks the acquirer at POST_KERNEL 0
 * until the release arrives (here the HP's VISION must never wait for the HE's sound handshake),
 * and has no way to stop an acquirer that began a step while the releasing core restarted.
 * I2C1 (camera) keeps using it.
 *
 * Every word has ONE writer, so no read-modify-write crosses cores:
 *   HE writes he_state / he_token / he_beat / he_regains / he_reclaims
 *   HP writes hp_state / hp_token / hp_beat / hp_aborts / hp_i2s_fu / hp_i2s_err / hp_acq
 * State words carry TR_BUS2_TAG (a cold SRAM0 reads as "none", never as a state) AND the low 12
 * bits of the lease token (tr_bus2_word()): "which state, for which lease" is ONE load, so no
 * second load needs ordering against the first (the Dekker edges below). The full token still
 * travels in he_token / hp_token.
 *
 *   HE: OWNS --(HP wants it, alive)--> OFFERED(T) --(HP claims T)--> CLAIMED(T) --(HP returns T)--> OWNS
 *                                         '--(no claim for 1 s: withdraw)--> OWNS
 *   HP: NONE -> WANT --(live offer T)--> BUS/HELD (the lease, T) --> RETURN(T) ... or --> WANT (abort)
 *
 *   - A token T is fresh per offer: a RETURN for any other token (a late write of a previous
 *     session) never gives the bus back. An HP that restarted (hp_state back to NONE at its
 *     PRE_KERNEL_1) or aborted (WANT) while CLAIMED releases the lease by itself.
 *   - An offer is only accepted when he_beat has MOVED since the HP began waiting: a dead HE's
 *     leftover offer (SRAM0 survives warm resets and debugger sessions) never moves it. The HE
 *     offers only to an HP whose hp_beat moves (a dead HP's stale WANT never takes the bus away).
 *   - Dekker at both edges: the HP enters every bus step as "hp_state = BUS, fence, THEN he_state
 *     must still be OFFER(T)"; the HE at boot (and when it withdraws an unclaimed offer) as
 *     "he_state = OWNS, fence, THEN hp_state != BUS". One of the two always sees the other, so an
 *     HE restart (or withdrawal) in the middle of a bring-up aborts it BEFORE the next access,
 *     and the HE waits (bounded) for a bus step already running.
 *   - The HP's I2C2 IRQ (NVIC line 134) is on ONLY between tr_bus2_hp_enter() and _leave() /
 *     _return(): a lease held across a step that does not use the bus (the CC3501E reset, the I2S3
 *     bring-up) has the line OFF, so an HE-only reset in that window cannot have its FIFO drained
 *     and INTR_MASK zeroed by the HP's i2c_dw_isr. The HP also hands the lease back across those
 *     stretches (sound/src/main.c leases only around the EEPROM read, steps 4-7 and step 11).
 *   - Liveness: the HP bumps hp_beat on every enter / leave / poll, and at least every ~100 ms
 *     while it holds the lease. A lease whose hp_beat has not moved for TR_BUS2_HP_DEAD_MS is dead:
 *     the HE stops the controller, clears the bus (ops->reclaim) and takes the bus back by itself.
 *   - Fail safe: no HP, or no WANT: the HE keeps the bus. No HE (or a HE without this glue): the
 *     HP waits forever, vision unaffected, no sound. An HP that is alive but never returns: the HE
 *     does not take the bus (the HP may be mid-transfer, and it beats); its HUD shows "5V -- mW".
 *
 * Pure C: barriers, clock and the two hardware actions (stop / re-arm the HE's I2C2) are the
 * caller's, so the state machine is host-tested (tests/host/test_bus2.c) with both cores simulated.
 *
 * WHERE. TR_MEM_BUS2 (tr_memmap.h) 0x0237FD40, 48 B, in the shared NC page. NOT 0x0237FC94: that
 * is the alp,i2c-handover flag of I2C1: the design this was ported from kept its record there,
 * which collides with it here.
 */
#ifndef TR_BUS2_H
#define TR_BUS2_H

#include <stdbool.h>
#include <stdint.h>

#include "tr_cam_view.h"
#include "tr_mbox.h"
#include "tr_memmap.h"

#define TR_BUS2_TAG       0x42320000u /* 'B2' */
#define TR_BUS2_TAG_MASK  0xFFFF0000u
#define TR_BUS2_ST_MASK   0xFu /* state word: bits 3..0 state, 15..4 token & 0xFFF */
#define TR_BUS2_TOK_SHIFT 4u
#define TR_BUS2_TOK_MASK  0xFFFu

/* he_state (HE-written) */
#define TR_BUS2_HE_OWNS  0u
#define TR_BUS2_HE_OFFER 1u
/* hp_state (HP-written) */
#define TR_BUS2_HP_NONE   0u
#define TR_BUS2_HP_WANT   1u /* waiting for an offer (hp_beat moves) */
#define TR_BUS2_HP_BUS    2u /* in a step that uses I2C2 / GPIO5 */
#define TR_BUS2_HP_HELD   3u /* holds the lease, between bus steps (SPI1 / LP-GPIO / I2S3 only) */
#define TR_BUS2_HP_RETURN 4u /* gave the bus back (hp_token = the token returned) */

#define TR_BUS2_OFFER_TIMEOUT_MS 1000u /* HE: withdraw an offer nobody claimed */
#define TR_BUS2_HP_DEAD_MS       2000u /* HE: a lease whose hp_beat is silent this long is dead */
#define TR_BUS2_HP_FRESH_MS      200u  /* HE: hp_beat must have moved this recently */
#define TR_BUS2_HE_BOOT_WAIT_MS  500u  /* HE boot: wait for a running HP bus step, at most */
#define TR_BUS2_HP_POLL_MS       20u   /* HP: hp_beat period while waiting */

typedef struct {
	uint32_t he_state;
	uint32_t he_token;
	uint32_t he_beat;
	uint32_t he_regains; /* leases given back, withdrawn or reclaimed */
	uint32_t hp_state;
	uint32_t hp_token;
	uint32_t hp_beat;
	uint32_t hp_aborts;   /* bring-up steps stopped by a claim */
	uint32_t hp_i2s_fu;   /* I2S3 TX FIFO underruns seen while streaming */
	uint32_t hp_i2s_err;  /* I2S3 TX block found parked */
	uint32_t hp_acq;      /* leases completed */
	uint32_t he_reclaims; /* leases taken back from an HP that stopped beating */
} tr_bus2_t;

_Static_assert(sizeof(tr_bus2_t) == 48u, "tr_bus2_t is 12 words on the wire");
_Static_assert(TR_MEM_BUS2 % 64u == 0u, "the record is 64-B aligned");
_Static_assert(TR_MEM_BUS2 >= TR_MEM_CAM_VIEW + sizeof(tr_cam_view_t),
               "the lease record starts after the camera view descriptor");
_Static_assert(TR_MEM_BUS2 + sizeof(tr_bus2_t) <= TR_MHU0_WINDOW_LO,
               "the lease record sits inside the shared NC page below the MHU0 window");
_Static_assert(TR_MEM_BUS2 >= TR_MEM_I2C1_HANDOVER + 12u && TR_MEM_BUS2 >= TR_MEM_HP_DBG + 0x68u,
               "the lease record is clear of the I2C1 handover words and hp_dbg");
_Static_assert(TR_MEM_BUS2 != TR_MEM_I2C1_HANDOVER,
               "I2C1 and I2C2/GPIO5 leases are different words");

/* A state word: tag | token & 0xFFF | state. */
static inline uint32_t tr_bus2_word(uint32_t state, uint32_t token)
{
	return TR_BUS2_TAG | ((token & TR_BUS2_TOK_MASK) << TR_BUS2_TOK_SHIFT) |
	       (state & TR_BUS2_ST_MASK);
}

/* What the state machine asks of its core. now_ms / sleep_ms: any monotonic clock (the HE's boot
 * claim runs where the kernel clock is up, POST_KERNEL 0). give(): stop this core's I2C2 (IRQ off,
 * controller disabled) -- called BEFORE an offer is published. take(): re-arm it (controller
 * reset, pending IRQ cleared, bus recovered if stuck) -- called after the bus is back.
 * reclaim(): the holder is dead -- stop the controller and clear the bus; take() follows. HP
 * ignores give / take / reclaim / nonce. */
typedef struct {
	void (*barrier)(void);
	void (*give)(void *ctx);
	void (*take)(void *ctx);
	void (*reclaim)(void *ctx);
	int64_t (*now_ms)(void *ctx);
	void (*sleep_ms)(void *ctx, uint32_t ms);
	void *ctx;
} tr_bus2_ops_t;

/* ---- HE ------------------------------------------------------------------------------------ */
enum { TR_BUS2_ST_OWNS, TR_BUS2_ST_OFFERED, TR_BUS2_ST_CLAIMED };

typedef struct {
	uint8_t  st;
	uint32_t token;
	uint32_t seen_hp_beat;
	int64_t  hp_moved_ms; /* when hp_beat last changed */
	bool     hp_moved;
	int64_t  t_offer;
	int64_t  t_hp_seen;  /* the lease's last sign of life: its claim, or hp_beat moving */
	bool     boot_stale; /* he_boot: hp_state was BUS but nothing moved -- a dead HP's leftover */
	uint32_t leased;     /* leases the HP actually held, counted from the evidence (a claim seen,
	                      * or a RETURN of the offered token seen) even when the whole lease passed
	                      * between two ticks and CLAIMED was never observed */
} tr_bus2_he_t;

/* Boot (before any I2C2 driver init): zero the counters this core owns (he_regains, he_reclaims:
 * SRAM0 powers up with garbage and survives warm resets), void the offer, fence, THEN wait, bounded, while the HP is
 * in a bus step. Returns true when a LIVE HP (hp_beat or hp_state moved during the wait) was still
 * in the step at the timeout; the HE proceeds regardless. A BUS that never moves is a dead HP's
 * leftover (SRAM0 survives warm resets): returns false with he->boot_stale set (a warning). */
bool tr_bus2_he_boot(tr_bus2_he_t        *he,
                     volatile tr_bus2_t  *r,
                     const tr_bus2_ops_t *o,
                     uint32_t             timeout_ms);

/* Every frame, from the HE's main loop (never from an ISR): at most one transition. `nonce` makes
 * the next offer's token (any per-call value). Does not touch I2C2 itself except through
 * o->give() / o->take(). */
void tr_bus2_he_tick(tr_bus2_he_t        *he,
                     volatile tr_bus2_t  *r,
                     const tr_bus2_ops_t *o,
                     uint32_t             nonce);

/* What one tick changed, for the console, from the evidence only: `before` = he->st and
 * `leased_before` = he->leased taken before the tick. NULL when nothing changed. A lease that began
 * and ended between two ticks (OFFERED -> OWNS with he->leased moved) is reported as a lease, never
 * as a bare "back on the HE"; an OFFERED -> OWNS with no lease is a withdrawn offer. */
const char *tr_bus2_he_event(const tr_bus2_he_t *he, uint8_t before, uint32_t leased_before);

static inline bool tr_bus2_he_owns_bus(const tr_bus2_he_t *he)
{
	return he->st == TR_BUS2_ST_OWNS;
}

/* ---- HP ------------------------------------------------------------------------------------ */
typedef struct {
	uint32_t base_beat; /* he_beat when the wait began: an offer counts once it moved */
	uint32_t token;     /* the offer being leased */
} tr_bus2_hp_t;

/* PRE_KERNEL_1: zero the counters this core owns (hp_aborts, hp_i2s_fu, hp_i2s_err, hp_acq: SRAM0
 * powers up with garbage) and forget whatever a previous session left (the HE, if it holds a lease
 * for it, sees NONE and takes its bus back). Each core writes only its own words. */
void tr_bus2_hp_boot(volatile tr_bus2_t *r, void (*barrier)(void));

/* Begin waiting for the bus (also after an abort, and for a runtime amp write). */
void tr_bus2_hp_want(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void));

/* One poll (every TR_BUS2_HP_POLL_MS): hp_beat++; true when a live offer stands (hp->token set).
 * It does NOT start using the bus: tr_bus2_hp_enter() does. */
bool tr_bus2_hp_poll(tr_bus2_hp_t *hp, volatile tr_bus2_t *r);

/* Entering a step that uses I2C2 / GPIO5: hp_token, fence, hp_state = BUS, fence, THEN the offer
 * must still stand (one load of he_state). Bumps hp_beat. The caller arms its I2C2 IRQ only after
 * this returns true. false = the HE took the bus back: do not touch it, tr_bus2_hp_abort(). */
bool tr_bus2_hp_enter(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void));

/* Leaving a bus step for steps on SPI1 / LP-GPIO / I2S3 only (the lease stays). The caller disarms
 * its I2C2 IRQ first. Bumps hp_beat. */
void tr_bus2_hp_leave(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void));

/* Bring-up done: this core's I2C2 IRQ is off, nothing more on the bus -- give it back. */
void tr_bus2_hp_return(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void));

/* The lease was lost before an access: back to waiting for a NEW live offer. */
void tr_bus2_hp_abort(tr_bus2_hp_t *hp, volatile tr_bus2_t *r, void (*barrier)(void));

#endif /* TR_BUS2_H */
