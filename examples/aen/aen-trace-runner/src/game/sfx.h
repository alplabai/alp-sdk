/* src/game/sfx.h -- which game moments make a sound.
 *
 * tr_sfx_watch() diffs the game state against the previous frame's and
 * returns the TR_AEV_* events (src/ipc/tr_aring.h) that happened in
 * between; main.c pushes them into the HE -> HP ring. Pure, so the host test
 * drives it with scripted states.
 *
 * Not emitted here: TR_AEV_GAME_OVER (main.c sends it when the crash
 * sequence has played out, which this per-frame diff cannot see). A live
 * wire ahead (P4b) sends TR_AEV_WIRE every TR_SFX_WIRE_TICKS with
 * tr_game_wire_level() as the intensity.
 */
#ifndef TR_SFX_H
#define TR_SFX_H

#include "../ipc/tr_aring.h"
#include "state.h"

#define TR_SFX_MAX_EVENTS \
	8u /* per call; worst case is 8 (music, attract, jump, duck, pickup, footstep, wire, crash) */
/* wire hum/crackle re-sent while one is near: every 12 FRAMES (0.3 s) at any
 * game pace -- the SFX lasts ~0.45 s, so the hum stays continuous */
#define TR_SFX_WIRE_TICKS ((12u * TR_GAME_PACE_Q8 + 255u) / 256u)
#define TR_SFX_STEP_TICKS \
	(TR_RUN_CYCLE_TICKS / \
	 2u) /* footstep at each touch-down of the run cycle (6.7/s at the 20 steps/s play pace) */
_Static_assert(2u * TR_SFX_STEP_TICKS == TR_RUN_CYCLE_TICKS,
               "footsteps must land on the run cycle's two touch-downs");

typedef struct {
	bool     primed, attract, airborne, ducking, crashed;
	uint32_t tick, score;
	uint8_t  foot;
} tr_sfx_watch_t;

void tr_sfx_watch_init(tr_sfx_watch_t *w);

/* Events since the previous call, written to out[] (room for
 * TR_SFX_MAX_EVENTS); returns how many. A new run (tick went backwards)
 * resets the per-run state silently. `combo`: the multiplier the HUD shows
 * (score.h tr_score_t.combo, 1..TR_COMBO_MAX after a pickup) -- a pickup's
 * TR_AEV_PICKUP param is combo - 1, so its pitch climbs with the x2..x5 the
 * player sees and drops with it (miss, crash, lapse). */
unsigned tr_sfx_watch(tr_sfx_watch_t  *w,
                      const tr_game_t *g,
                      uint8_t          combo,
                      bool             attract_on,
                      int16_t          track_h,
                      tr_aev_t        *out);

#endif /* TR_SFX_H */
