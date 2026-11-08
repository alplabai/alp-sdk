/* tests/host/test_step.c */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../src/game/state.h"

static tr_intent_t move(int8_t d)
{
	tr_intent_t i = tr_intent_none();
	i.lane_delta  = d;
	i.source      = TR_INPUT_VISION;
	return i;
}

int main(void)
{
	tr_game_t g;

	/* Starts alive, centre lane, no score. */
	tr_game_init(&g, 1234u);
	assert(g.alive && g.lane == 1 && g.score == 0);

	/* Lane changes clamp at the edges rather than wrapping. */
	tr_game_step(&g, move(-1), 1280);
	assert(g.lane == 0);
	tr_game_step(&g, move(-1), 1280);
	assert(g.lane == 0);
	tr_game_step(&g, move(+1), 1280);
	tr_game_step(&g, move(+1), 1280);
	assert(g.lane == 2);
	tr_game_step(&g, move(+1), 1280);
	assert(g.lane == 2);

	/*
	 * A jump lifts the runner for exactly TR_AIR_TICKS - 1 ticks: the timer is
	 * armed at TR_AIR_TICKS but decremented in the same tick it's set.
	 */
	tr_game_init(&g, 1u);
	tr_intent_t j = tr_intent_none();
	j.jump        = true;
	tr_game_step(&g, j, 1280);
	assert(g.airborne);
	for (int k = 0; k < TR_AIR_TICKS - 2; k++) {
		tr_game_step(&g, tr_intent_none(), 1280);
		assert(g.airborne);
	}
	tr_game_step(&g, tr_intent_none(), 1280);
	assert(!g.airborne);

	/*
	 * Jumping over a low obstacle does not kill -- including the tick right
	 * after it clears the collision band.  Regression for a band that was
	 * twice the width of one tick's travel, which re-tested a cleared
	 * obstacle on the following tick against that tick's (by-then-false)
	 * airborne flag and killed the player anyway.
	 *
	 * The fixture starts at y=0 -- on spawn()'s real grid (TR_SPAWN_Y -1540
	 * is a whole number of steps above it, so band alignment is the same) --
	 * rather than an arbitrary position, and the jump is timed off the band's real
	 * approach distance, not a hand-picked offset from the band. Whole-branch
	 * review F1: the previous version of this fixture seeded `.y` from `ry +
	 * TR_SCROLL_PX * (TR_AIR_TICKS - 1)` = 1338, a geometry spawn() can never
	 * produce (below the bottom of a 1280-tall panel, and under the
	 * since-reversed scroll direction) -- it proved a jump could clear an
	 * obstacle given an approach distance the shipped game never gives a
	 * player, not the property it claimed to. Still hits the same tightest
	 * boundary as before: the jump starts exactly late enough that the
	 * obstacle reaches the band on the LAST airborne tick.
	 */
	{
		const int16_t track_h = 1280;
		int16_t       ry      = tr_runner_ground_y(track_h);
		/* First tick a real (y=0) spawn reaches the band: band width equals
		 * one tick's travel exactly, so ceil(ry / TR_SCROLL_PX) is the tick
		 * that lands inside it (see the scroll-direction test below, which
		 * proves this arithmetic against the actual step()). */
		int ticks_to_band = (ry + TR_SCROLL_PX - 1) / TR_SCROLL_PX;
		/*
		 * Airborne holds for TR_AIR_TICKS - 1 calls starting with the jump
		 * call itself (the timer is armed AND decremented the same tick --
		 * see state.h's TR_AIR_TICKS comment), i.e. calls
		 * [jump_call, jump_call + TR_AIR_TICKS - 2]. The airborne/ducking
		 * decrement in tr_game_step() runs BEFORE that tick's collision
		 * check, so the LAST call still cleared by the jump is
		 * jump_call + TR_AIR_TICKS - 2, not - 1: solving
		 * jump_call + TR_AIR_TICKS - 2 == ticks_to_band gives this.
		 */
		int jump_call = ticks_to_band - (TR_AIR_TICKS - 2);

		assert(jump_call >= 1); /* the approach must be long enough to jump at all */

		tr_game_init(&g, 7u);
		memset(g.ents, 0, sizeof(g.ents));
		g.ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 1, .y = 0, .low = true };

		for (int k = 1; k < jump_call; k++) {
			tr_game_step(&g, tr_intent_none(), track_h);
			assert(g.alive);
		}
		tr_game_step(&g, j, track_h); /* jump begins right before the obstacle reaches the band. */
		assert(g.alive && g.airborne);
		/*
		 * A bounded run past the collision tick, not "until ents[0] frees" --
		 * this obstacle's own despawn tick (well past the band, at
		 * track_h / TR_SCROLL_PX) can coincide with a periodic spawn() tick,
		 * which would immediately refill slot 0 with an unrelated entity and
		 * make the loop condition track the wrong entity's fate instead of
		 * concluding. A handful of ticks past ticks_to_band is enough to
		 * prove the jump actually cleared it.
		 */
		for (int k = jump_call + 1; k <= ticks_to_band + 5; k++) {
			tr_game_step(&g, tr_intent_none(), track_h);
			assert(g.alive);
		}
	}

	/* A ground obstacle in the runner's lane kills; jumping over it does not.
	 * y=0: on the real spawn() grid (TR_SPAWN_Y is a multiple of
	 * TR_SCROLL_PX), not an arbitrary position. */
	tr_game_init(&g, 7u);
	memset(g.ents, 0, sizeof(g.ents));
	g.ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 1, .y = 0, .low = true };
	while (g.alive && g.ents[0].kind != TR_ENT_FREE) {
		tr_game_step(&g, tr_intent_none(), 1280);
	}
	assert(!g.alive);

	/*
	 * Scroll-direction regression (whole-branch review F1): an entity's
	 * first on-panel position and the position where it reaches the
	 * collision band must be consistent with scrolling DOWN from spawn()'s
	 * y=0 toward tr_runner_ground_y() near the bottom -- not the reverse.
	 * This is the assertion the review says would have caught the bug: the
	 * old code spawned near the bottom and scrolled UP, so an entity's
	 * first position was already past the band instead of far above it.
	 */
	{
		const int16_t track_h = 1280;
		int16_t       ry      = tr_runner_ground_y(track_h);

		tr_game_init(&g, 19u);
		memset(g.ents, 0, sizeof(g.ents));
		/* lane 2 (the runner starts at lane 1): stays an untouched obstacle
		 * the whole way through the band, so its y can be inspected freely
		 * without the collision/clear logic consuming or scoring it. */
		g.ents[0] =
		    (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 2, .y = TR_SPAWN_Y, .low = true };

		assert(g.ents[0].y < ry); /* spawn()'s real position is well above the band, not past it */

		int ticks = 0;

		while (g.ents[0].y < ry) {
			tr_game_step(&g, tr_intent_none(), track_h);
			ticks++;
			assert(ticks < track_h); /* must reach the band, not run away from it forever */
		}
		/* Arrived inside the one-tick-wide band from above (increasing y),
		 * after a real approach from the spawn row (241 ticks for a
		 * 1280-tall panel -- ceil((1104 + 1540)/11); 141 from the P3c row
		 * -440, 101 from y 0, 62 at the pre-P3c 18 px/tick) -- not the
		 * 4-tick warning the reversed scroll produced. */
		assert(g.ents[0].y >= ry && g.ents[0].y < ry + TR_SCROLL_PX);
		assert(ticks == (ry - TR_SPAWN_Y + TR_SCROLL_PX - 1) / TR_SCROLL_PX &&
		       ticks >= 241); /* never less warning */

		/* and spawn() really puts a new part on that row */
		tr_game_init(&g, 19u);
		memset(g.ents, 0, sizeof(g.ents));
		for (int k = 0; k < TR_SPAWN_TICKS && g.ents[0].kind == TR_ENT_FREE; k++) {
			tr_game_step(&g, tr_intent_none(), track_h);
		}
		assert(g.ents[0].kind != TR_ENT_FREE && g.ents[0].y == TR_SPAWN_Y && TR_SPAWN_Y == -1540);
	}

	/*
	 * An obstacle kills exactly when it reaches the runner's drawn
	 * position (tr_runner_ground_y()) and not one tick before -- the
	 * regression for task-11-review.md finding 12 (the collision line used
	 * to be hardcoded to the top of the panel, 1,104 px from where the
	 * runner is actually drawn).
	 */
	{
		const int16_t track_h = 1280;
		int16_t       ry      = tr_runner_ground_y(track_h);

		tr_game_init(&g, 11u);
		memset(g.ents, 0, sizeof(g.ents));
		g.ents[0] = (tr_entity_t){
			.kind = TR_ENT_OBSTACLE, .lane = 1, .y = (int16_t)(ry - 2 * TR_SCROLL_PX), .low = true
		};
		tr_game_step(
		    &g, tr_intent_none(), track_h); /* now at ry - TR_SCROLL_PX: still one tick out. */
		assert(g.alive);
		tr_game_step(&g, tr_intent_none(), track_h); /* now at ry: exactly the runner's position. */
		assert(!g.alive);
	}

	/*
	 * An obstacle that has already scrolled past the runner's drawn
	 * position never kills again, even though it stays in the runner's
	 * lane all the way down the screen.
	 */
	{
		const int16_t track_h = 1280;
		int16_t       ry      = tr_runner_ground_y(track_h);

		tr_game_init(&g, 13u);
		memset(g.ents, 0, sizeof(g.ents));
		g.ents[0] = (tr_entity_t){
			.kind = TR_ENT_OBSTACLE, .lane = 1, .y = (int16_t)(ry + 5 * TR_SCROLL_PX), .low = true
		};
		for (int k = 0; k < 10; k++) {
			tr_game_step(&g, tr_intent_none(), track_h);
			assert(g.alive);
		}
	}

	/*
	 * The runner's drawn y and the collision band agree for a 1280-tall
	 * panel -- pins the number so a future change to TR_RUNNER_H or
	 * TR_RUNNER_GROUND_MARGIN in state.h is a loud, intentional diff here
	 * rather than a silent mismatch between step.c and render.c.
	 */
	assert(tr_runner_ground_y(1280) ==
	       1104); /* 1280 - TR_RUNNER_H(96) - TR_RUNNER_GROUND_MARGIN(80) */

	/* A pickup in the lane scores and frees its slot. y=0: the real spawn()
	 * position, not an arbitrary one -- and the score check below is tight
	 * enough (== 10, the pickup award, not just > 0) to fail if the pickup
	 * were consumed by the generic "survived past the far edge" +1 bonus
	 * instead of actually being collected. */
	tr_game_init(&g, 9u);
	memset(g.ents, 0, sizeof(g.ents));
	g.ents[0] = (tr_entity_t){ .kind = TR_ENT_PICKUP, .lane = 1, .y = 0, .low = true };
	while (g.alive && g.ents[0].kind != TR_ENT_FREE) {
		tr_game_step(&g, tr_intent_none(), 1280);
	}
	assert(g.alive && g.score == 10u);

	/*
	 * The same seed replays identically -- the game is deterministic.
	 * Whole-branch review F12: comparing only two scalars between two
	 * structs seeded identically and stepped with identical inputs by the
	 * same code cannot fail (there is no state left that could differ), so
	 * it cannot distinguish a deterministic implementation from a broken
	 * one. Two changes make it real:
	 *
	 * 1. A RECORDED expected value, captured from this implementation
	 *    (score=25, tick=401 -- 158 before P3c's slower world, 261 before
	 *    its spawn row moved out to -440, 301 before -1540 -- the
	 *    player dies with no input, as expected,
	 *    and score/tick then freeze; see tr_game_step()'s `if (!g->alive)
	 *    return;`). A future change to the RNG, the spawn policy, or the
	 *    scroll geometry that shifts this is expected to update the pin
	 *    deliberately, not pass silently.
	 * 2. Every field of both runs, not just two of nine scalars plus a
	 *    16-entry array -- field by field rather than memcmp(), because
	 *    tr_game_init() sets each scalar individually (not a whole-struct
	 *    zero) and tr_game_t's inter-field padding is not guaranteed equal
	 *    between two separately-declared automatic locals.
	 */
	/*
	 * P6 crash: the fatal tick records which slot hit (crashed, hit,
	 * crash_ticks 0); tr_game_crash_step() then gives exactly
	 * TR_CRASH_FRAMES - 1 more frames with the world frozen, and never
	 * runs on a run that did not crash (or after a restart).
	 */
	{
		const int16_t track_h = 1280;
		int16_t       ry      = tr_runner_ground_y(track_h);

		tr_game_init(&g, 21u);
		assert(!g.crashed && !tr_game_crash_step(&g));
		memset(g.ents, 0, sizeof(g.ents));
		g.ents[5] = (tr_entity_t){
			.kind = TR_ENT_OBSTACLE, .lane = 1, .y = (int16_t)(ry - 1), .low = false
		};
		tr_game_step(&g, tr_intent_none(), track_h);
		assert(!g.alive && g.crashed && g.hit == 5u && g.crash_ticks == 0u);

		tr_game_t frozen = g;
		int       frames = 0;

		while (tr_game_crash_step(&g)) {
			frames++;
			assert(g.crash_ticks == (uint8_t)frames);
			tr_game_step(&g, tr_intent_none(), track_h); /* a dead run does not move */
			assert(g.ents[5].y == frozen.ents[5].y && g.tick == frozen.tick);
		}
		assert(frames == TR_CRASH_FRAMES - 1 && g.crash_ticks == TR_CRASH_FRAMES - 1);
		assert(!tr_game_crash_step(&g) && g.crash_ticks == TR_CRASH_FRAMES - 1);
		tr_game_init(&g, 21u);
		assert(!g.crashed && g.crash_ticks == 0u && !tr_game_crash_step(&g));
	}

	/*
	 * P4b live wires. Spawn mix: the same seed spawns the same kinds, and
	 * about a fifth of the obstacles are wires, of both heights (every
	 * spawn is freed at once, so the run never dies and 2,000 spawns are
	 * seen). The kind sequence is pinned like the replay below.
	 */
	{
		tr_game_t w1, w2;
		unsigned  kinds[4] = { 0 }, wlow = 0, n = 0;
		uint32_t  crc = 0;

		tr_game_init(&w1, 77u);
		tr_game_init(&w2, 77u);
		while (n < 2000u) {
			tr_game_step(&w1, tr_intent_none(), 1280);
			tr_game_step(&w2, tr_intent_none(), 1280);
			assert(w1.alive);
			for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
				tr_entity_t *e = &w1.ents[i];

				if (e->kind == TR_ENT_FREE) {
					continue;
				}
				assert(e->kind == w2.ents[i].kind && e->lane == w2.ents[i].lane &&
				       e->low == w2.ents[i].low);
				kinds[e->kind]++;
				wlow += e->kind == TR_ENT_WIRE && e->low;
				crc = crc * 31u + (uint32_t)e->kind * 7u + e->lane * 3u + e->low;
				n++;
				e->kind = w2.ents[i].kind = TR_ENT_FREE;
			}
		}
		unsigned obst = kinds[TR_ENT_OBSTACLE] + kinds[TR_ENT_WIRE];

		assert(kinds[TR_ENT_WIRE] * 100u >= obst * 12u && kinds[TR_ENT_WIRE] * 100u <= obst * 28u);
		assert(wlow > 0u && wlow < kinds[TR_ENT_WIRE]);
		assert(tr_ent_is_obstacle(TR_ENT_WIRE) && tr_ent_is_obstacle(TR_ENT_OBSTACLE) &&
		       !tr_ent_is_obstacle(TR_ENT_PICKUP) && !tr_ent_is_obstacle(TR_ENT_FREE));
		printf("spawn mix: %u obstacles, %u wires (%u low), %u pickups, kinds crc %08x\n",
		       kinds[TR_ENT_OBSTACLE],
		       kinds[TR_ENT_WIRE],
		       wlow,
		       kinds[TR_ENT_PICKUP],
		       (unsigned)crc);
		assert(crc == 0x12ae7934u);
	}

	/*
	 * Wire collision: same rules as the other obstacles -- a jump clears a
	 * low wire, a duck a high one; the wrong move (or none) crashes on it.
	 */
	for (int low = 0; low <= 1; low++) {
		for (int act = 0; act < 3; act++) { /* none, jump, duck */
			const int16_t track_h = 1280;
			int16_t       ry      = tr_runner_ground_y(track_h);
			tr_intent_t   in      = tr_intent_none();

			tr_game_init(&g, 31u);
			memset(g.ents, 0, sizeof(g.ents));
			g.ents[3] = (tr_entity_t){
				.kind = TR_ENT_WIRE, .lane = 1, .y = (int16_t)(ry - 3 * TR_SCROLL_PX), .low = low
			};
			in.jump = act == 1;
			in.duck = act == 2;
			for (int k = 0; k < 6; k++) {
				tr_game_step(&g, k == 0 ? in : tr_intent_none(), track_h);
			}
			bool clear = low ? act == 1 : act == 2;

			assert(g.alive == clear && g.crashed == !clear);
			assert(clear || g.hit == 3u);
		}
	}

	/*
	 * The sound hook (P10's TR_AEV_WIRE intensity): 0 with no wire, 255
	 * for a wire at the runner line in its lane, less farther up the
	 * track, less in another lane; a wire already past the runner is
	 * silent.
	 */
	{
		const int16_t track_h = 1280;
		int16_t       ry      = tr_runner_ground_y(track_h);

		tr_game_init(&g, 5u);
		memset(g.ents, 0, sizeof(g.ents));
		g.ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE, .lane = 1, .y = ry, .low = true };
		assert(tr_game_wire_level(&g, track_h) == 0u);
		g.ents[1] = (tr_entity_t){ .kind = TR_ENT_WIRE, .lane = 1, .y = ry, .low = true };
		assert(tr_game_wire_level(&g, track_h) == 255u);
		g.ents[1].y = (int16_t)(ry / 2);
		uint8_t mid = tr_game_wire_level(&g, track_h);

		assert(mid > 0u && mid < 255u);
		g.ents[1].lane = 0;
		assert(tr_game_wire_level(&g, track_h) < mid);
		g.ents[1].lane = 1;
		g.ents[1].y    = (int16_t)(ry + TR_SCROLL_PX - 1); /* still in the runner band */
		assert(tr_game_wire_level(&g, track_h) == 255u);
		g.ents[1].y = (int16_t)(ry + TR_SCROLL_PX);
		assert(tr_game_wire_level(&g, track_h) == 0u);
		assert(tr_game_wire_level(&g, (int16_t)(TR_RUNNER_H + TR_RUNNER_GROUND_MARGIN)) ==
		       0u); /* ry 0: no divide */
	}

	/*
	 * The longer approach (TR_SPAWN_Y -1540) changed only the approach:
	 * a part still spawns every TR_SPAWN_TICKS steps (1 s at the play
	 * pace, 20 steps/s), none dropped for want of a free ents[] slot, with
	 * at most TR_ENT_ALIVE_MAX alive at once. Obstacles about to hit are
	 * moved a lane over so the run lives; that touches no spawn.
	 */
	{
		const int16_t track_h = 1280, ry = tr_runner_ground_y(track_h);
		tr_game_t     g;
		uint32_t      spawns = 0, most = 0;

		assert(TR_SPAWN_TICKS == 20 && TR_GAME_PACE_Q8 == 128u); /* 1 s between spawns, as ever */
		assert(TR_ENT_ALIVE_MAX == 13 && TR_ENT_ALIVE_MAX <= TR_MAX_ENTITIES);
		tr_game_init(&g, 7u);
		for (uint32_t t = 1; t <= 4000; t++) {
			uint32_t alive = 0, fresh = 0;

			for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
				tr_entity_t *e = &g.ents[i];

				if (tr_ent_is_obstacle(e->kind) && e->lane == g.lane && e->y + TR_SCROLL_PX >= ry &&
				    e->y < ry) {
					e->lane = (uint8_t)((e->lane + 1u) % TR_LANES);
				}
			}
			tr_game_step(&g, tr_intent_none(), track_h);
			assert(g.alive && g.tick == t);
			for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
				alive += g.ents[i].kind != TR_ENT_FREE;
				fresh += g.ents[i].kind != TR_ENT_FREE && g.ents[i].y == TR_SPAWN_Y;
			}
			assert(fresh == (t % TR_SPAWN_TICKS == 0u)); /* on the beat, never dropped */
			spawns += fresh;
			most = alive > most ? alive : most;
		}
		assert(spawns == 4000u / TR_SPAWN_TICKS && most <= TR_ENT_ALIVE_MAX);
		printf("step: %u spawns in 4000 steps, one every %u (1 s), at most %u of %u slots alive\n",
		       (unsigned)spawns,
		       (unsigned)TR_SPAWN_TICKS,
		       (unsigned)most,
		       (unsigned)TR_MAX_ENTITIES);
	}

	tr_game_t a, b;
	tr_game_init(&a, 42u);
	tr_game_init(&b, 42u);
	for (int k = 0; k < 500; k++) {
		tr_game_step(&a, tr_intent_none(), 1280);
		tr_game_step(&b, tr_intent_none(), 1280);
	}
	/* P3c: 158 -> 261 (11 px/tick, spawn every 20) -> 301 (spawn row -440) -> 401 (spawn row -1540): the
	 * same parts kill the same idle run with the same score, exactly the 100 steps of added approach later */
	assert(a.score == 25u && a.tick == 401u);
	assert(a.lane == b.lane && a.airborne == b.airborne && a.air_ticks == b.air_ticks &&
	       a.ducking == b.ducking && a.duck_ticks == b.duck_ticks && a.score == b.score &&
	       a.alive == b.alive && a.tick == b.tick && a.rng == b.rng);
	for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
		assert(a.ents[i].kind == b.ents[i].kind && a.ents[i].lane == b.ents[i].lane &&
		       a.ents[i].y == b.ents[i].y && a.ents[i].low == b.ents[i].low);
	}
	/* A jump asked while the runner is ducking or airborne is held for
	 * TR_JUMP_BUFFER_TICKS steps and taken the first step the runner is free;
	 * asked earlier than that, it is dropped. */
	{
		tr_intent_t dk = tr_intent_none(), jp = tr_intent_none();

		dk.duck = true;
		jp.jump = true;
		for (int early = 0; early < 2; early++) {
			tr_game_init(&g, 7u);
			tr_game_step(&g, dk, 1280); /* ducking, duck_ticks = TR_DUCK_TICKS - 1 */
			assert(g.ducking);
			/* idle until `left` steps remain of the duck, then ask */
			int idle = early ? 2 : TR_DUCK_TICKS - 1 - TR_JUMP_BUFFER_TICKS + 1;

			for (int k = 0; k < idle; k++) {
				tr_game_step(&g, tr_intent_none(), 1280);
			}
			tr_game_step(&g, jp, 1280);
			for (int k = 0; k < TR_DUCK_TICKS && !g.airborne; k++) {
				tr_game_step(&g, tr_intent_none(), 1280);
			}
			if (early) {
				assert(!g.airborne); /* asked too soon: forgotten */
			} else {
				assert(g.airborne); /* asked in the last steps of the duck: taken after it */
			}
			if (!g.alive) {
				break;
			}
		}
		/* ...and the same while airborne: asked with 2 steps of the jump left it is taken the
		 * step after landing; asked with 5 left it has expired by then. */
		for (int left = 5; left >= 2; left -= 3) {
			tr_game_init(&g, 7u);
			tr_game_step(&g, jp, 1280);
			assert(g.airborne);
			while (g.air_ticks > left) {
				tr_game_step(&g, tr_intent_none(), 1280);
			}
			tr_game_step(&g, jp, 1280);
			while (g.airborne) {
				tr_game_step(&g, tr_intent_none(), 1280); /* until it lands */
			}
			tr_game_step(&g, tr_intent_none(), 1280);
			assert(g.airborne == (left == 2));
		}
	}
	return 0;
}
