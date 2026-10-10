/* src/game/state.c */
#include "state.h"

void tr_game_init(tr_game_t *g, uint32_t seed)
{
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		g->ents[i] = (tr_entity_t){ 0 };
	}
	g->lane        = TR_LANES / 2u;
	g->airborne    = false;
	g->air_ticks   = 0;
	g->ducking     = false;
	g->duck_ticks  = 0;
	g->jump_wait   = 0;
	g->score       = 0;
	g->alive       = true;
	g->crashed     = false;
	g->hit         = 0;
	g->crash_ticks = 0;
	g->ev          = 0;
	g->ev_lane     = 0;
	g->dodge_lane  = 0;
	g->dodge_ticks = 255;
	g->tick        = 0;
	/* Seed 0 would lock the LCG at zero for ever; fold it to a non-zero constant. */
	g->rng = seed ? seed : 0x2f6e2b1u;
}
