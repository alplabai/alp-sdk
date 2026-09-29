/* src/game/react.c -- see react.h. */
#include "react.h"

#include "../ipc/tr_mbox.h" /* TR_REACT_* */
#include "score.h"           /* TR_COMBO_MAX */

void tr_react_init(tr_react_t *r)
{
	*r = (tr_react_t){TR_REACT_NONE, 1, 0u, 0u, TR_REACT_MAX_US};
}

/* Priority: the enum order (pass 1 < pickup 2 < near 3 < combo 4). */
_Static_assert(TR_REACT_PASS < TR_REACT_PICKUP && TR_REACT_PICKUP < TR_REACT_NEAR && TR_REACT_NEAR < TR_REACT_COMBO,
	       "tr_mbox.h TR_REACT_* is the priority order");

void tr_react_step(tr_react_t *r, const tr_game_t *g, uint8_t combo)
{
	uint8_t k = TR_REACT_NONE;

	/* milestones: x3, and the top multiplier (score.h x2..x5) */
	if ((g->ev & TR_EV_PICKUP) && combo != r->combo && (combo == 3u || combo == TR_COMBO_MAX)) {
		k = TR_REACT_COMBO;
	} else if (g->ev & TR_EV_NEAR) {
		k = TR_REACT_NEAR;
	} else if (g->ev & TR_EV_PICKUP) {
		k = TR_REACT_PICKUP;
	} else if ((g->ev & TR_EV_PASS) && r->us >= TR_REACT_PASS_GAP_US) {
		k = TR_REACT_PASS;
	}
	r->combo = combo;
	if (k == TR_REACT_NONE || (k < r->kind && r->us < TR_REACT_HOLD_US)) {
		return;
	}
	r->kind = k;
	r->us   = 0u;
	r->seq++;
	if (g->ev & (TR_EV_PASS | TR_EV_NEAR)) {
		/* where it went by; in the runner's own lane, alternate */
		r->side = g->ev_lane < g->lane ? (int8_t)-1 : g->ev_lane > g->lane ? (int8_t)1 : (r->seq & 1u) ? (int8_t)1 : (int8_t)-1;
	}
}

void tr_react_frame(tr_react_t *r)
{
	r->us = r->us < TR_REACT_MAX_US - TR_PANEL_PERIOD_US ? r->us + TR_PANEL_PERIOD_US : TR_REACT_MAX_US;
}
