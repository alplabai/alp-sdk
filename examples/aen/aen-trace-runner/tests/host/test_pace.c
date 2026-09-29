/* tests/host/test_pace.c -- the global game pace (state.h TR_GAME_PACE_Q8):
 * tr_game_pace() steps evenly, never twice a frame; input held over the
 * frames between steps (tr_intent_merge) is not lost; a paced run is the
 * same game, step for step, as an unpaced one fed the same held inputs --
 * collisions are judged per step, none skipped. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/state.h"

/* Every field that matters, compared. */
static int same(const tr_game_t *a, const tr_game_t *b)
{
	if (a->tick != b->tick || a->score != b->score || a->lane != b->lane || a->alive != b->alive ||
	    a->airborne != b->airborne || a->ducking != b->ducking || a->rng != b->rng ||
	    a->crashed != b->crashed) {
		return 0;
	}
	for (int i = 0; i < TR_MAX_ENTITIES; i++) {
		if (a->ents[i].kind != b->ents[i].kind || a->ents[i].y != b->ents[i].y ||
		    a->ents[i].lane != b->ents[i].lane) {
			return 0;
		}
	}
	return 1;
}

static uint32_t lcg(uint32_t *r)
{
	*r = *r * 1664525u + 1013904223u;
	return *r >> 8;
}

/* A paced run over `frames` frames at speed_q16 with scripted per-frame
 * input; records the input actually stepped with. Returns steps. */
static int paced(tr_game_t *g, uint32_t speed_q16, int frames, uint32_t seed, tr_intent_t *log)
{
	uint32_t    phase = 0, r = seed;
	tr_intent_t held  = tr_intent_none();
	int         steps = 0;

	tr_game_init(g, 42u);
	for (int f = 0; f < frames; f++) {
		tr_intent_t in = tr_intent_none();
		uint32_t    x  = lcg(&r);

		in.lane_delta = (int8_t)((x & 15u) == 0 ? -1 : (x & 15u) == 1 ? 1 : 0);
		in.jump       = (x & 63u) == 7u;
		in.duck       = (x & 63u) == 9u;
		held          = tr_intent_merge(held, in);
		if (tr_game_pace(&phase, speed_q16)) {
			tr_game_step(g, held, 1280);
			log[steps++] = held;
			held         = tr_intent_none();
		}
		assert(phase < 65536u);
	}
	return steps;
}

int main(void)
{
	static tr_intent_t log[4000];

	/* 1. Even pacing: at 0.5x every other frame steps; at 1.0x every
	 * frame; never two steps in one frame (at most one per call). */
	{
		uint32_t p = 0;
		int      n = 0;

		for (int f = 0; f < 1000; f++) {
			int s = tr_game_pace(&p, 32768u);

			assert(s == (f & 1)); /* 0.5: steps on the odd frames */
			n += s;
		}
		assert(n == 500);
		p = 0, n = 0;
		for (int f = 0; f < 1000; f++) {
			n += tr_game_pace(&p, 65536u);
		}
		assert(n == 1000 && p == 0u);
		/* 0.5x of 40 steps/s at any panel rate: Q16 per frame x TR_PANEL_HZ
		 * frames/s == TR_GAME_PACE_Q8 x 256 per 40 Hz frame x 40, to rounding */
		assert(TR_GAME_PACE_Q8 == 128u);
		assert(TR_PLAY_SPEED_Q16 * TR_PANEL_HZ + TR_PANEL_HZ / 2u >= TR_GAME_PACE_Q8 * 256u * 40u &&
		       TR_PLAY_SPEED_Q16 * TR_PANEL_HZ <= TR_GAME_PACE_Q8 * 256u * 40u + TR_PANEL_HZ / 2u);
	}

	/* 2. Held input through tr_play_frame(): a jump on a frame that does
	 * not step is carried to the next step; lane moves land on their own
	 * frame, ONE lane a frame -- a stride (+1, +1) crosses two lanes on two
	 * frames, back-and-forth (-1, +1) ends where it began, and a +1 on a
	 * step frame from lane 0 reaches lane 1, not 2. At exactly 0.5 a frame
	 * (32768, the 40 Hz play pace), so the step frames are known at any
	 * TR_PANEL_HZ. */
	{
		tr_intent_t h = tr_intent_none(), j = tr_intent_none(), l = tr_intent_none(),
		            r = tr_intent_none(), st;
		tr_game_t   g;
		uint32_t    ph = 0;

		j.jump       = true;
		l.lane_delta = -1;
		r.lane_delta = 1;
		h            = tr_intent_merge(h, j);
		h            = tr_intent_merge(h, tr_intent_none());
		assert(h.jump && !h.duck && h.lane_delta == 0);

		tr_game_init(&g, 5u);
		g.lane = 0;
		h      = tr_intent_none();
		assert(!tr_play_frame(&g, &h, r, &ph, 32768u, &st) && g.lane == 1); /* frame 0 */
		assert(tr_play_frame(&g, &h, r, &ph, 32768u, &st) && g.lane == 2);  /* frame 1 steps */
		assert(st.lane_delta == 0 && h.lane_delta == 0);
		tr_game_step(&g, st, 1280);
		assert(g.lane == 2); /* the step moves no lane of its own */
		assert(!tr_play_frame(&g, &h, l, &ph, 32768u, &st) && g.lane == 1);
		assert(tr_play_frame(&g, &h, r, &ph, 32768u, &st) && g.lane == 2);

		/* one lane a frame: from lane 0, a +1 on a STEP frame, then the step */
		tr_game_init(&g, 5u);
		g.lane = 0, ph = 65535u, h = tr_intent_none();
		assert(tr_play_frame(&g, &h, r, &ph, 65536u, &st));
		tr_game_step(&g, st, 1280);
		assert(g.lane == 1 && h.lane_delta == 0 && st.lane_delta == 0);
		/* a jump on a non-step frame is stepped with at the next step */
		tr_game_init(&g, 5u);
		ph = 0, h = tr_intent_none();
		assert(!tr_play_frame(&g, &h, j, &ph, 32768u, &st));
		assert(tr_play_frame(&g, &h, tr_intent_none(), &ph, 32768u, &st) && st.jump);
		tr_game_step(&g, st, 1280);
		assert(g.airborne);
	}

	/* 2b. The crash sequence: one crash tick a frame at ANY pace -- it
	 * ends after TR_CRASH_FRAMES - 1 frames (the fatal frame was the first),
	 * phase 0 throughout. (The first paced version zeroed the phase before
	 * pacing and never ended: the A32 build hung on GAME OVER.) */
	{
		for (int k = 0; k < 2; k++) {
			tr_game_t g;
			uint32_t  ph     = 12345u;
			int       frames = 0;

			tr_game_init(&g, 1u + (uint32_t)k);
			g.alive = false, g.crashed = true, g.crash_ticks = 0;
			while (tr_game_crash_frame(&g, &ph)) {
				assert(ph == 0u);
				assert(++frames < 1000);
			}
			assert(frames == TR_CRASH_FRAMES - 1 && g.crash_ticks == TR_CRASH_FRAMES - 1);
			/* a run that did not crash: no sequence */
			tr_game_init(&g, 1u);
			assert(!tr_game_crash_frame(&g, &ph));
		}
	}

	/* 3. Same game: a run at 0.5x, 1.0x -- each replayed unpaced from
	 * the inputs it stepped with, ends in the identical state (the crash
	 * on the same step); 0.5x takes twice the frames for the same steps. */
	for (int k = 0; k < 2; k++) {
		uint32_t  speed = k ? 65536u : TR_PLAY_SPEED_Q16;
		tr_game_t a, b;
		int       steps = paced(&a, speed, 3000, 7u, log);

		tr_game_init(&b, 42u);
		for (int i = 0; i < steps; i++) {
			tr_game_step(&b, log[i], 1280);
		}
		assert(same(&a, &b));
		assert(steps == (int)((3000u * (uint64_t)speed) >> 16)); /* 40 Hz 1500, 30 Hz 2000 */
		printf("pace %s: %d steps in 3000 frames, run %s at tick %u, score %u\n",
		       k ? "1.0" : "0.5",
		       steps,
		       a.alive ? "alive" : "over",
		       (unsigned)a.tick,
		       (unsigned)a.score);
	}

	/* 4. No collision skipped: an obstacle in the lane with no input hits
	 * on the same step at any pace (the band is one step wide, and a
	 * frame runs at most one step). */
	for (int k = 0; k < 2; k++) {
		uint32_t  speed = k ? 65536u : TR_PLAY_SPEED_Q16, phase = 0;
		tr_game_t g;
		int16_t   ry    = tr_runner_ground_y(1280);
		int       steps = 0;

		tr_game_init(&g, 3u);
		memset(g.ents, 0, sizeof(g.ents));
		g.ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE,
			                       .lane = g.lane,
			                       .y    = (int16_t)(ry - 20 * TR_SCROLL_PX),
			                       .low  = true };
		for (int f = 0; f < 200 && g.alive; f++) {
			if (tr_game_pace(&phase, speed)) {
				tr_game_step(&g, tr_intent_none(), 1280);
				steps++;
			}
		}
		assert(!g.alive && g.crashed && steps == 20);
	}
	/* 5. Play input a frame at a time (tr_play_frame): at 0.5x a lane
	 * move is applied on the frame it is seen -- not at the next step --
	 * and the step then judges the obstacle against the new lane: the same
	 * obstacle kills a runner that did not move and misses one that moved
	 * on the frame between the steps. Same frames, same game. (0.5 a frame
	 * exactly, as in 2.) */
	{
		int16_t ry = tr_runner_ground_y(1280);

		for (int moved = 0; moved < 2; moved++) {
			tr_game_t   g, g2;
			tr_intent_t held = tr_intent_none(), step_in, right = tr_intent_none();
			uint32_t    ph    = 0;
			int         steps = 0;

			right.lane_delta = 1;
			for (int copy = 0; copy < 2; copy++) {
				tr_game_t *x = copy ? &g2 : &g;

				tr_game_init(x, 9u);
				memset(x->ents, 0, sizeof(x->ents));
				x->lane    = 1;
				x->ents[0] = (tr_entity_t){ .kind = TR_ENT_OBSTACLE,
					                        .lane = 1,
					                        .y    = (int16_t)(ry - TR_SCROLL_PX),
					                        .low  = true };
				held = tr_intent_none(), ph = 0, steps = 0;
				for (int f = 0; f < 6; f++) {
					tr_intent_t in = (moved && f == 0)
					                     ? right
					                     : tr_intent_none(); /* frame 0 does not step at 0.5x */

					if (tr_play_frame(x, &held, in, &ph, 32768u, &step_in)) {
						tr_game_step(x, step_in, 1280);
						steps++;
					} else if (f == 0) {
						assert(x->lane == (moved ? 2 : 1)); /* moved on its own frame */
					}
				}
			}
			assert(steps == 3 && same(&g, &g2));
			assert(moved ? g.alive : !g.alive);
		}
		printf("pace 0.5: lane applied on the frame seen, collision judged at the step\n");
	}
	printf("test_pace: ok\n");
	return 0;
}
