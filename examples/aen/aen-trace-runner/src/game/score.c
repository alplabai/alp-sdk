/* src/game/score.c -- see score.h. */
#include "score.h"

#include <string.h>

void tr_score_init(tr_score_t *s)
{
	memset(s, 0, sizeof(*s)); /* whole struct, padding included: test_score.c memcmp()s two */
}

void tr_score_run_start(tr_score_t *s)
{
	s->score      = 0u;
	s->metres     = 0u;
	s->pickup_pts = 0u;
	s->combo      = 0u;
	s->new_best   = 0u;
	s->hs_done    = 0u;
	s->hs_top     = 0u;
	s->combo_tick = 0u;
}

void tr_score_step(tr_score_t *s, const tr_game_t *g)
{
	if (s->combo != 0u &&
	    g->tick - s->combo_tick >= TR_COMBO_WINDOW + (g->ev & TR_EV_PICKUP ? 1u : 0u)) {
		s->combo = 0u; /* lapsed: this pickup (if any) came too late for it */
	}
	if (g->ev & TR_EV_PICKUP) {
		if (s->combo < TR_COMBO_MAX) {
			s->combo++;
		}
		s->combo_tick = g->tick;
		s->popup_mult = s->combo;
		s->popup_pts  = (uint16_t)(10u * s->combo);
		s->popup_hs   = 0u;
		s->pickup_pts += s->popup_pts;
		s->popup_seq++;
	}
	if (g->ev & (TR_EV_MISS | TR_EV_CRASH)) {
		s->combo = 0u;
	}
	if (g->alive) {
		s->metres = g->tick * TR_SCROLL_PX / TR_PX_PER_M;
	}
	s->score = s->metres + s->pickup_pts;
	if (s->hs_top != 0u && !s->hs_done && s->score > s->hs_top &&
	    g->tick - s->combo_tick >= TR_HS_POPUP_GAP) {
		s->hs_done    = 1u;
		s->popup_hs   = 1u;
		s->popup_pts  = 0u;
		s->popup_mult = 0u;
		s->popup_seq++;
	}
}

void tr_score_run_end(tr_score_t *s, bool counts)
{
	s->new_best = counts && s->score > s->best;
	if (s->new_best) {
		s->best = s->score;
	}
}

uint32_t tr_score_best_now(const tr_score_t *s)
{
	return s->score > s->best ? s->score : s->best;
}
