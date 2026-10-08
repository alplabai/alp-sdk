/* src/game/tilt.h */
#ifndef TR_TILT_H
#define TR_TILT_H

#include <stdbool.h>
#include <stdint.h>

#include "intent.h"
#include "panel_hz.h"

/*
 * Tilt as a BODY input: the pure half of the IMU path. platform/imu.c only
 * reads the BMI323 and hands over rest-zeroed Q8 g samples (256 == 1 g); every
 * decision about what a sample MEANS lives here, host-tested
 * (tests/host/test_tilt.c).
 *
 * Two jobs:
 *  1. tr_tilt_intent(): sample -> lane change / jump / duck, with hysteresis.
 *  2. tr_tilt_step(): the camera-absent booth loop. TR_MODE_ATTRACT plays
 *     itself until a deliberate tilt gesture takes over; a walk-away hands
 *     the screen back to attract, which re-engages on the next gesture.
 *
 * TR_TILT_TAKEOVER (CMake option, default OFF): tilt is a BENCH/DEV input.
 * The exhibition board is fixed to a mount and nobody picks it up
 * (docs/2026-09-22-exhibition-requirements.md), so the default build keeps
 * TR_MODE_ATTRACT as pure self-play: tr_tilt_step() never engages. ON
 * enables job 2 for bench tilt tests.
 *
 * All thresholds are CALIBRATION KNOBS (#ifndef-guarded, override with -D).
 * Tick-counted ones are one call a frame, written as 40 Hz frames and
 * scaled to TR_PANEL_HZ (panel_hz.h) so the real time holds at a 30 Hz
 * panel too; TR_RENDER=M55 (TICK_MS 33, ~30 Hz) runs them ~1.3x longer.
 * Q8 -> angle, board level at rest: sin(theta) = q8 / 256.
 */

#ifndef TR_TILT_TAKEOVER
#define TR_TILT_TAKEOVER 0
#endif

/* Steering sign for the board's mounting: on a bench E1M-AEN803 (on the EVK
 * carrier) the BMI323 x axis reads POSITIVE for a tilt to the LEFT, so a
 * positive x_q8 must move the runner left (lane_delta -1). Only the lane
 * direction flips; the engage / level / dead-zone logic uses |x| and is
 * sign-agnostic. Pitch (jump / duck) is unchanged until confirmed on the
 * board. */
#ifndef TR_TILT_STEER_SIGN
#define TR_TILT_STEER_SIGN (-1)
#endif

/* Steer/pitch hysteresis: fire past EDGE, re-arm only back inside DEAD.
 * The 28-count gap (~0.11 g) is what stops a hand hovering at the edge from
 * chattering lanes. P3c (maintainer: "needs too much tilt"): EDGE 90 -> 48,
 * DEAD 15 -> 20. 2026W36-0009's recorded rest noise spikes to 18 Q8: under DEAD
 * (a spike never fires and never needs filtering -- a filter would only add
 * a frame of lag), well under EDGE. */
#ifndef TR_TILT_DEAD_Q8
#define TR_TILT_DEAD_Q8 20 /* ~0.08 g, ~4.5 deg: hand tremor + the rest noise (<= 18). */
#endif
#ifndef TR_TILT_EDGE_Q8
#define TR_TILT_EDGE_Q8 48 /* ~0.19 g, ~10.8 deg: a deliberate tilt. */
#endif

/* Takeover gesture: tilt past ENGAGE on either axis, starting from level,
 * held ENGAGE_TICKS in a row. 0.5 s (20 frames at 40 Hz):
 * longer than any knock or table bump (a spike is 1-3 samples), short
 * enough to feel instant to someone who means it. */
#ifndef TR_TILT_ENGAGE_Q8
#define TR_TILT_ENGAGE_Q8 \
	64 /* ~0.25 g, ~14.5 deg: firmer than a steer, so a bump cannot start a game */
#endif
#ifndef TR_TILT_ENGAGE_TICKS
#define TR_TILT_ENGAGE_TICKS TR_HZ_FRAMES(20)
#endif

/* Walk-away: no lane change/jump/duck for IDLE_TICKS mid-run returns to
 * attract. 7.5 s (300 frames at 40 Hz) -- an endless runner
 * throws an obstacle every couple of seconds, so a real player gestures far
 * more often than this. */
#ifndef TR_TILT_IDLE_TICKS
#define TR_TILT_IDLE_TICKS TR_HZ_FRAMES(300)
#endif

/* At game over, back to attract instead of a new run if the dead run saw no
 * gesture for this long: 2.25 s (90 frames at 40 Hz). */
#ifndef TR_TILT_OVER_IDLE_TICKS
#define TR_TILT_OVER_IDLE_TICKS TR_HZ_FRAMES(90)
#endif

/* Characters to pick from (P16; == tr_mbox.h TR_CHAR_N, asserted there). */
#define TR_TILT_CHARS 4

typedef struct {
	bool     playing;       /**< Tilt drives a real run; false = attract self-play. */
	bool     armed;         /**< Seen level since entering attract: the engage hold may count. */
	bool     steer_latched; /**< One lane change per tilt, re-armed inside the dead zone. */
	bool     pitch_latched; /**< Same, jump/duck -- independent of steer. */
	uint16_t hold_ticks;    /**< Consecutive ticks past ENGAGE while armed. */
	uint32_t idle_ticks;    /**< Ticks since the last gesture while playing. */
	/* Bench counters (read over SWD -- main.c's global `tr_tilt`). */
	uint32_t engages;   /**< attract -> tilt play transitions. */
	uint32_t walkaways; /**< tilt play -> attract transitions (timeout or idle game over). */
	uint32_t gestures;  /**< lane changes + jumps + ducks decoded. */
	uint8_t character; /**< P16: the selected character (tr_mbox.h TR_CHAR_*), picked in attract. */
	int8_t
	     pick_dir; /**< A pick flick under way: -1 / +1 once past EDGE, committed back at level. */
	bool pick_ready; /**< Seen level since the last pick: the next flick may count. */
	uint32_t picks;  /**< Bench counter: characters picked. */
	bool     pinned; /**< A pick holds against the demo's cycling until the player walks away. */
} tr_tilt_t;

typedef enum {
	TR_TILT_STAY = 0,        /**< No transition this tick. */
	TR_TILT_START_RUN,       /**< Attract -> tilt play: caller starts a fresh run. */
	TR_TILT_BACK_TO_ATTRACT, /**< Walk-away: caller starts a fresh attract run. */
} tr_tilt_event_t;

void tr_tilt_init(tr_tilt_t *t);

/* Sample -> intent (source TR_INPUT_TILT), steer on x, jump/duck on y. Used
 * directly by TR_MODE_TILT (bench build) and by tr_tilt_step() in play. */
tr_intent_t tr_tilt_intent(tr_tilt_t *t, int16_t x_q8, int16_t y_q8);

/* One TR_MODE_ATTRACT tick. With TR_TILT_TAKEOVER, attract also picks the
 * character (t->character, P16): a flick left / right and back to level
 * picks the previous / next one; it persists until tr_tilt_init() (boot).
 * `*out` is the tilt intent to apply while
 * t->playing (tr_intent_none() otherwise -- the caller uses
 * tr_attract_intent() then). The engage tick itself emits no gesture: the
 * held tilt is latched, so it cannot also throw a lane change into the fresh
 * run. */
tr_tilt_event_t tr_tilt_step(tr_tilt_t *t, int16_t x_q8, int16_t y_q8, tr_intent_t *out);

/* An attract demo run is over (P16, every build): the demo moves on to the
 * next character, so a passer-by sees all of them -- unless a pick pinned
 * one for the next player run (a walk-away unpins it). The character a
 * player run starts with is whatever attract shows at the takeover. */
void tr_tilt_demo_over(tr_tilt_t *t);

/* Call at game over while t->playing. True = the player has gone (no gesture
 * for TR_TILT_OVER_IDLE_TICKS): back to attract instead of a new run. */
bool tr_tilt_run_over(tr_tilt_t *t);

#endif /* TR_TILT_H */
