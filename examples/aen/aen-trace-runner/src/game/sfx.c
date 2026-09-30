/* src/game/sfx.c -- see sfx.h. */
#include "sfx.h"

#include <string.h>

#include "../ipc/tr_mbox.h" /* tr_game_crash_kind(), TR_CRASH_KIND_* */

void tr_sfx_watch_init(tr_sfx_watch_t *w)
{
	memset(w, 0, sizeof(*w));
}

static unsigned emit(tr_aev_t *out, unsigned n, uint8_t kind, uint8_t param)
{
	out[n] = (tr_aev_t){ .kind = kind, .param = param };
	return n + 1u;
}

unsigned tr_sfx_watch(tr_sfx_watch_t  *w,
                      const tr_game_t *g,
                      uint8_t          combo,
                      bool             attract_on,
                      int16_t          track_h,
                      tr_aev_t        *out)
{
	unsigned n = 0;

	if (!w->primed || attract_on != w->attract) {
		n = emit(out, n, TR_AEV_MUSIC, attract_on ? TR_MUSIC_ATTRACT : TR_MUSIC_PLAY);
		if (attract_on) {
			n = emit(out, n, TR_AEV_ATTRACT, 0);
		}
	}
	if (!w->primed || g->tick < w->tick) { /* first call, or a fresh run */
		w->primed   = true;
		w->attract  = attract_on;
		w->airborne = g->airborne;
		w->ducking  = g->ducking;
		w->crashed  = g->crashed;
		w->tick     = g->tick;
		w->score    = g->score;
		w->foot     = 0;
		return n;
	}
	w->attract = attract_on;

	if (g->tick != w->tick) {
		if (g->airborne && !w->airborne) {
			n = emit(out, n, TR_AEV_JUMP, 0);
		}
		if (g->ducking && !w->ducking) {
			n = emit(out, n, TR_AEV_DUCK, 0);
		}
		/* step.c: +10 per pickup, +1 per entity that scrolls off -- a
		 * pickup is the only way to gain 10 in one tick */
		if (g->score >= w->score + 10u) {
			n = emit(out, n, TR_AEV_PICKUP, combo != 0u ? (uint8_t)(combo - 1u) : 0u);
		}
		if (g->alive && !g->airborne && g->tick % TR_SFX_STEP_TICKS == 0u) {
			w->foot ^= 1u;
			n = emit(out, n, TR_AEV_FOOTSTEP, w->foot);
		}
		if (g->alive && g->tick % TR_SFX_WIRE_TICKS == 0u) {
			uint8_t lvl = tr_game_wire_level(g, track_h);
			if (lvl != 0u) {
				n = emit(out, n, TR_AEV_WIRE, lvl);
			}
		}
	}
	if (g->crashed && !w->crashed) {
		n = emit(out, n, TR_AEV_CRASH, tr_game_crash_kind(g)); /* LOW / HIGH / WIRE */
	}
	w->airborne = g->airborne;
	w->ducking  = g->ducking;
	w->crashed  = g->crashed;
	w->tick     = g->tick;
	w->score    = g->score;
	return n;
}
