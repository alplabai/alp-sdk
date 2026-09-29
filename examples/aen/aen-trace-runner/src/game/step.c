/* src/game/step.c */
#include "state.h"

/* Small LCG: deterministic across host and target, which is what the replay test checks. */
static uint32_t rng_next(tr_game_t *g)
{
	g->rng = g->rng * 1664525u + 1013904223u;
	return g->rng >> 8;
}

/*
 * Entities spawn ABOVE the top of the panel (TR_SPAWN_Y, -1540) and scroll DOWN
 * (increasing y) toward the runner, who stands near the BOTTOM
 * (tr_runner_ground_y(), see state.h). This matches tools/genart.py's
 * stated art model -- the runner faces up-screen and "the board scrolls
 * down past them" -- and is what gives the player a real approach: at
 * TR_SCROLL_PX=11 px/tick, tr_runner_ground_y(1280)=1104 is reached 241
 * steps after spawn (ceil((1104 + 1540)/11); -1540 is a multiple of 11 and
 * the band is one step wide, so exactly one step lands in it) -- 101 from
 * y 0. Spawning at track_h - 1 and scrolling
 * UP (the previous behaviour) put entities 175 px from the runner at
 * spawn -- 4 ticks of actual on-panel warning after paint()'s all-or-
 * nothing bounds check -- then had them travel 1,104 empty px away from a
 * runner they had already passed. See task-11-review.md (whole-branch
 * review) F1: this is not a cosmetic bug, it made the shipped game
 * average under 2 s of play with no input, which is what a player who
 * could not react produces.
 *
 * The 3D scene draws a part from its spawn row (depth ~21,700, the far end
 * of the visible road), growing and fading it in over its first steps
 * (r3d_scene.c tr_scene_ent_fog / tr_scene_ent_scale);
 * the M55 sprite renderer's paint() is all-or-nothing on panel bounds
 * (render.c), so there a part pops in at y 0 as before.
 */
static void spawn(tr_game_t *g)
{
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		if (g->ents[i].kind != TR_ENT_FREE) {
			continue;
		}
		uint32_t r      = rng_next(g);
		/* A quarter pickups; of the obstacles one in five is a live wire
		 * (P4b), from bits no other choice below reads. */
		g->ents[i].kind = !(r & 3u) ? TR_ENT_PICKUP : ((r >> 16) % 5u == 0u) ? TR_ENT_WIRE : TR_ENT_OBSTACLE;
		g->ents[i].lane = (uint8_t)((r >> 3) % TR_LANES);
		g->ents[i].y    = TR_SPAWN_Y;
		/* Two thirds of obstacles are low (jump); the rest are high (duck). */
		g->ents[i].low = ((r >> 7) % 3u) != 0u;
		return;
	}
}

static void steer(tr_game_t *g, int8_t lane_delta)
{
	uint8_t was = g->lane;

	/* Lane changes clamp; wrapping would let a player dodge by spamming one direction. */
	if (lane_delta < 0 && g->lane > 0u) {
		g->lane--;
	} else if (lane_delta > 0 && g->lane + 1u < TR_LANES) {
		g->lane++;
	}
	if (g->lane != was) {
		g->dodge_lane  = was; /* P16: an obstacle arriving there soon was dodged */
		g->dodge_ticks = 0u;
	}
}

/* P16: did the jump / duck clear it by a small margin -- started on one of
 * the last TR_NEAR_TICKS + 1 steps, or ending within TR_NEAR_TICKS? The
 * timers are the ticks left after this step's decrement (state.h). */
static bool near_clear(const tr_game_t *g, bool low)
{
	int left = low ? g->air_ticks : g->duck_ticks, full = (low ? TR_AIR_TICKS : TR_DUCK_TICKS) - 1;

	return left >= full - TR_NEAR_TICKS || left <= TR_NEAR_TICKS;
}

void tr_game_step(tr_game_t *g, tr_intent_t in, int16_t track_h)
{
	if (!g->alive) {
		return;
	}
	g->tick++;
	g->ev          = 0;
	g->dodge_ticks = g->dodge_ticks < 255u ? (uint8_t)(g->dodge_ticks + 1u) : 255u;

	steer(g, in.lane_delta);

	if (in.jump && !g->airborne && !g->ducking) {
		g->airborne  = true;
		g->air_ticks = TR_AIR_TICKS;
	}
	if (in.duck && !g->airborne && !g->ducking) {
		g->ducking    = true;
		g->duck_ticks = TR_DUCK_TICKS;
	}
	if (g->airborne && --g->air_ticks == 0u) {
		g->airborne = false;
	}
	if (g->ducking && --g->duck_ticks == 0u) {
		g->ducking = false;
	}

	/*
	 * Where the runner actually is -- see state.h's tr_runner_ground_y(),
	 * the one definition render.c's draw position also uses. This used to
	 * be hardcoded to y in [0, TR_SCROLL_PX) (the top of the panel), which
	 * had nothing to do with where the runner was drawn (near the bottom);
	 * see task-11-review.md finding 12.
	 */
	int16_t runner_y = tr_runner_ground_y(track_h);

	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		tr_entity_t *e = &g->ents[i];

		if (e->kind == TR_ENT_FREE) {
			continue;
		}
		e->y += TR_SCROLL_PX; /* Down the panel, from spawn() at the top toward the runner at the bottom. */

		/*
		 * The runner occupies the band [runner_y, runner_y + TR_SCROLL_PX):
		 * one tick's travel. Width must equal TR_SCROLL_PX exactly -- wider
		 * double-tests a cleared obstacle on the following tick (it isn't
		 * freed on a successful clear), re-checking it against the
		 * airborne/ducking flag of that later tick.
		 */
		bool at_runner = (e->y >= runner_y) && (e->y < runner_y + TR_SCROLL_PX);

		if (at_runner && e->kind == TR_ENT_PICKUP && e->lane != g->lane) {
			g->ev |= TR_EV_MISS; /* judged once: the band is one tick wide */
		}
		if (at_runner && e->lane == g->lane) {
			if (e->kind == TR_ENT_PICKUP) {
				g->score += 10u;
				g->ev |= TR_EV_PICKUP;
				e->kind = TR_ENT_FREE;
				continue;
			}
			/* Low obstacles are cleared by being airborne, high ones by ducking. */
			bool cleared = e->low ? g->airborne : g->ducking;

			if (!cleared) {
				g->alive   = false;
				g->crashed = true;
				g->ev |= TR_EV_CRASH;
				g->hit     = (uint8_t)i;
				return;
			}
			g->ev |= TR_EV_PASS | (near_clear(g, e->low) ? TR_EV_NEAR : 0u);
			g->ev_lane = e->lane;
		} else if (at_runner && tr_ent_is_obstacle(e->kind) && e->lane == g->dodge_lane &&
			   g->dodge_ticks <= TR_NEAR_TICKS) {
			g->ev |= TR_EV_PASS | TR_EV_NEAR; /* dodged at the last moment */
			g->ev_lane = e->lane;
		}
		if (e->y >= track_h) {
			e->kind = TR_ENT_FREE;
			g->score += 1u; /* Surviving past one costs nothing but is worth something. */
		}
	}

	if ((g->tick % TR_SPAWN_TICKS) == 0u) {
		spawn(g);
	}
}

uint8_t tr_game_wire_level(const tr_game_t *g, int16_t track_h)
{
	int32_t ry = tr_runner_ground_y(track_h), best = 0;

	if (ry <= 0) {
		return 0u; /* no track to hear it on */
	}
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		const tr_entity_t *e = &g->ents[i];

		/* Up to the end of step.c's runner band [ry, ry + TR_SCROLL_PX):
		 * a wire there is still at the runner, full volume. */
		if (e->kind != TR_ENT_WIRE || e->y >= ry + TR_SCROLL_PX || e->y < 0) {
			continue;
		}
		int32_t v = e->y >= ry ? 255 : 255 * (int32_t)e->y / ry;

		v = e->lane == g->lane ? v : v / 2;
		best = v > best ? v : best;
	}
	return (uint8_t)best;
}

bool tr_game_pace(uint32_t *phase_q16, uint32_t speed_q16)
{
	*phase_q16 += speed_q16;
	if (*phase_q16 < 65536u) {
		return false;
	}
	*phase_q16 -= 65536u;
	return true;
}

bool tr_play_frame(tr_game_t *g, tr_intent_t *held, tr_intent_t in, uint32_t *phase_q16, uint32_t speed_q16,
		   tr_intent_t *step_in)
{
	/* A lane move lands now, one lane (a frame never moves two); only the
	 * jump / duck wait for the step. */
	if (in.lane_delta != 0 && g->alive) {
		steer(g, in.lane_delta > 0 ? 1 : -1);
	}
	in.lane_delta = 0;
	*held         = tr_intent_merge(*held, in);
	if (!tr_game_pace(phase_q16, speed_q16)) {
		return false;
	}
	*step_in = *held;
	*held    = tr_intent_none();
	return true;
}

bool tr_game_crash_frame(tr_game_t *g, uint32_t *phase_q16)
{
	*phase_q16 = 0u;
	return tr_game_crash_step(g);
}

bool tr_game_crash_step(tr_game_t *g)
{
	if (!g->crashed || g->crash_ticks + 1u >= TR_CRASH_FRAMES) {
		return false;
	}
	g->crash_ticks++;
	return true;
}
