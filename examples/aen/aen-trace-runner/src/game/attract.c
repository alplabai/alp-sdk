/* src/game/attract.c */
#include "attract.h"

/* Small LCG, same shape as step.c's rng_next() but its OWN stream (a
 * tr_attract_t field, never tr_game_t.rng): watching an attract run must
 * never perturb the spawn sequence a real run started from the same seed
 * would have gotten. */
static uint32_t rng_next(uint32_t *rng)
{
	*rng = *rng * 1664525u + 1013904223u;
	return *rng >> 8;
}

void tr_attract_init(tr_attract_t *a)
{
	*a = (tr_attract_t){ 0 };
	a->rng =
	    0x6a09e667u; /* arbitrary nonzero seed -- see rng_next()'s LCG, which locks at 0 forever from 0. */
}

tr_attract_ev_t
tr_attract_step(tr_attract_t *a, tr_track_t *track, bool player_present, uint8_t game_lane)
{
	if (!a->active) {
		if (player_present) {
			a->idle_ticks = 0u;
		} else if (++a->idle_ticks >= TR_ATTRACT_ENTER_TICKS) {
			tr_attract_enter(a);
			return TR_ATTRACT_ENTERED;
		}
		return TR_ATTRACT_STAY;
	}
	if (!player_present) {
		a->join_ticks = 0u; /* the lobby starts over */
		return TR_ATTRACT_STAY;
	}
	if (++a->join_ticks < TR_ATTRACT_JOIN_TICKS) {
		return TR_ATTRACT_STAY;
	}
	a->active     = false;
	a->idle_ticks = 0u;
	a->join_ticks = 0u;
	/* RESYNC SITE: leaving attract into a real run is a discontinuity
	 * exactly like leaving pause (mode.c) or a run reset -- see
	 * tr_track_resync()'s contract in vision/track.h. */
	tr_track_resync(track, game_lane);
	return TR_ATTRACT_LEFT;
}

void tr_attract_enter(tr_attract_t *a)
{
	a->active     = true;
	a->idle_ticks = 0u;
	a->join_ticks = 0u;
}

bool tr_attract_joining(const tr_attract_t *a)
{
	return a->active && a->join_ticks > 0u;
}

bool tr_attract_run_over(tr_attract_t *a, bool player_present)
{
	if (!player_present) {
		tr_attract_enter(a);
	}
	return a->active;
}

tr_intent_t tr_attract_intent(tr_attract_t *a, const tr_game_t *g, int16_t track_h)
{
	tr_intent_t out = tr_intent_none();

	out.source = TR_INPUT_ATTRACT;

	/* A slot's react/miss roll belongs to whatever entity currently
	 * occupies it -- clear it the instant the slot frees, so the NEXT
	 * occupant (a different entity entirely) gets its own fresh roll
	 * instead of inheriting a decision made about something else. */
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		if (g->ents[i].kind == TR_ENT_FREE) {
			a->acted[i] = false;
		}
	}

	int16_t runner_y = tr_runner_ground_y(track_h); /* the one shared collision line, see state.h */
	bool    reacted  = false;

	/*
	 * React to the nearest not-yet-decided obstacle in the CURRENT lane,
	 * once it is within TR_ATTRACT_REACT_TICKS of the runner. Exactly one
	 * decision is made per entity slot (a->acted[]) -- rolled the first
	 * tick it enters range -- so TR_ATTRACT_MISS_IN below is a single coin
	 * flip per obstacle, not TR_ATTRACT_REACT_TICKS independent ones that
	 * would make a "miss" astronomically unlikely.
	 */
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		const tr_entity_t *e = &g->ents[i];

		if (!tr_ent_is_obstacle(e->kind) || e->lane != g->lane || e->y >= runner_y || a->acted[i]) {
			continue;
		}
		int16_t ticks_out = (int16_t)((runner_y - e->y) / TR_SCROLL_PX);

		if (ticks_out > TR_ATTRACT_REACT_TICKS) {
			continue; /* not yet in range */
		}
		a->acted[i] = true;
		reacted     = true;
		if ((rng_next(&a->rng) % TR_ATTRACT_MISS_IN) != 0u) {
			out.jump = e->low;
			out.duck = !e->low;
		}
		/* else: the deliberate miss -- see TR_ATTRACT_MISS_IN's comment. */
		break; /* one obstacle reacted to per tick; jump/duck are mutually exclusive anyway. */
	}

	if (!reacted && (rng_next(&a->rng) % TR_ATTRACT_WANDER_IN) == 0u) {
		/* Nothing urgent: wander sideways every so often so the demo
		 * visibly changes lanes between obstacles instead of only ever
		 * moving in reaction to one -- see TR_ATTRACT_WANDER_IN's comment. */
		out.lane_delta = (rng_next(&a->rng) & 1u) ? (int8_t)1 : (int8_t)-1;
	}

	return out;
}

bool tr_attract_pace(uint32_t *phase_q16)
{
	return tr_game_pace(phase_q16, TR_ATTRACT_SPEED_Q16);
}

void tr_lobby_init(tr_lobby_t *l)
{
	*l = (tr_lobby_t){ 0 };
}

void tr_lobby_demo_over(tr_lobby_t *l)
{
	l->frames = TR_LOBBY_FRAMES;
	l->shown  = 0u;
}

bool tr_lobby_frame(tr_lobby_t *l, bool attract)
{
	if (attract && !l->was_attract) {
		tr_lobby_demo_over(l); /* just came into attract */
	}
	if (!attract) {
		l->frames = 0u; /* a player: at once */
	}
	l->was_attract = attract;
	l->standing    = l->frames > 0u;
	if (l->standing) {
		l->idle_us = l->shown * TR_PANEL_PERIOD_US;
		l->shown++;
		l->frames--;
	}
	return l->standing;
}
