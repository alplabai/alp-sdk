/* tests/host/test_react.c -- P16 reaction triggers: the game's pass /
 * near-miss events (game/step.c) and the reaction the frame packet carries
 * (game/react.c), deterministic and frame-rate independent. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/react.h"
#include "../../src/game/score.h"
#include "../../src/game/state.h"
#include "../../src/ipc/tr_mbox.h"

#define H 1280

static tr_intent_t none(void)
{
	return tr_intent_none();
}

static tr_intent_t act(int jump, int duck, int8_t lane)
{
	tr_intent_t i = tr_intent_none();

	i.jump = jump, i.duck = duck, i.lane_delta = lane;
	return i;
}

/* A fresh game with one obstacle in `lane`, `ticks` steps from the runner
 * line (it lands in step.c's band on the ticks-th step). */
static void one_obstacle(tr_game_t *g, uint8_t lane, bool low, int ticks)
{
	tr_game_init(g, 7u);
	g->tick         = 1; /* no spawn on the steps below (tick % TR_SPAWN_TICKS) */
	g->ents[0].kind = TR_ENT_OBSTACLE;
	g->ents[0].lane = lane;
	g->ents[0].low  = low;
	g->ents[0].y    = (int16_t)(tr_runner_ground_y(H) - ticks * TR_SCROLL_PX);
}

/* Steps until the obstacle is judged (`ticks` steps), the action on step
 * `at` (1-based); returns the events of the judging step. */
static uint8_t run(tr_game_t *g, int ticks, int at, tr_intent_t a)
{
	for (int k = 1; k <= ticks; k++) {
		tr_game_step(g, k == at ? a : none(), H);
		assert(g->alive);
	}
	return g->ev;
}

int main(void)
{
	tr_game_t g;

	/* 1. Clearing an obstacle in the runner's lane is a pass; late (the
	 * jump on the last TR_NEAR_TICKS + 1 steps) or at the very end of the
	 * air time it is a near miss; mid-jump it is not. Same for a duck. */
	{
		int n_near = 0;

		for (int at = 1; at <= 12; at++) {
			uint8_t ev;

			one_obstacle(&g, 1, true, 12);
			ev = run(&g, 12, at, act(1, 0, 0));
			assert(ev & TR_EV_PASS);
			assert(g.ev_lane == 1);
			/* airborne ticks left at the judging step: 13 - (12 - at) */
			int left = TR_AIR_TICKS - 1 - (12 - at);
			int near = left >= TR_AIR_TICKS - 1 - TR_NEAR_TICKS || left <= TR_NEAR_TICKS;

			assert(!!(ev & TR_EV_NEAR) == near);
			n_near += near;
		}
		assert(n_near ==
		       TR_NEAR_TICKS +
		           2); /* 3 late jumps + the one landing (2 ticks left) this range reaches */
		one_obstacle(&g, 1, false, 12);
		assert((run(&g, 12, 12, act(0, 1, 0)) & (TR_EV_PASS | TR_EV_NEAR)) ==
		       (TR_EV_PASS | TR_EV_NEAR));
		one_obstacle(&g, 1, false, 12);
		assert((run(&g, 12, 6, act(0, 1, 0)) & (TR_EV_PASS | TR_EV_NEAR)) == TR_EV_PASS);
	}

	/* 2. A dodge: the obstacle reaches the line in the lane the runner left
	 * TR_NEAR_TICKS steps ago or less -- a near miss on that side; left
	 * long before, or never in it, nothing. */
	{
		one_obstacle(&g, 1, true, 10);
		assert((run(&g, 10, 9, act(0, 0, 1)) & (TR_EV_PASS | TR_EV_NEAR)) ==
		       (TR_EV_PASS | TR_EV_NEAR));
		assert(g.ev_lane == 1 && g.lane == 2);
		one_obstacle(&g, 1, true, 10);
		assert((run(&g, 10, 2, act(0, 0, 1)) & (TR_EV_PASS | TR_EV_NEAR)) == 0);
		one_obstacle(&g, 0, true, 10);
		assert((run(&g, 10, 9, act(0, 0, 1)) & (TR_EV_PASS | TR_EV_NEAR)) == 0);
		/* the window's edge: left TR_NEAR_TICKS steps before the judging
		 * step counts, one more does not */
		one_obstacle(&g, 1, true, 10);
		assert(run(&g, 10, 10 - TR_NEAR_TICKS, act(0, 0, 1)) & TR_EV_NEAR);
		one_obstacle(&g, 1, true, 10);
		assert((run(&g, 10, 9 - TR_NEAR_TICKS, act(0, 0, 1)) & (TR_EV_PASS | TR_EV_NEAR)) == 0);
	}

	/* 3. The reaction: priority combo > near > pickup > pass; a new pass
	 * waits TR_REACT_PASS_GAP_US after any reaction; the clock runs in real
	 * time at either panel rate and saturates. */
	{
		tr_react_t r;

		tr_react_init(&r);
		assert(r.kind == TR_REACT_NONE);
		g.ev = TR_EV_PASS, g.ev_lane = 0, g.lane = 1;
		tr_react_step(&r, &g, 0);
		assert(r.kind == TR_REACT_PASS && r.side == -1 && r.seq == 1 && tr_react_ms(&r) == 0);
		tr_react_frame(&r);
		assert(tr_react_ms(&r) == TR_PANEL_PERIOD_US / 1000u);
		g.ev = TR_EV_PASS;
		tr_react_step(&r, &g, 0);
		assert(r.seq == 1); /* too soon */
		g.ev = TR_EV_PICKUP;
		tr_react_step(&r, &g, 1);
		assert(r.kind == TR_REACT_PICKUP && r.seq == 2);
		tr_react_frame(&r);
		g.ev = TR_EV_PICKUP; /* an equal one replaces it: the clock restarts */
		tr_react_step(&r, &g, 2);
		assert(r.kind == TR_REACT_PICKUP && r.seq == 3 && tr_react_ms(&r) == 0);
		g.ev = TR_EV_PICKUP;
		tr_react_step(&r, &g, 3);
		assert(r.kind == TR_REACT_COMBO && r.seq == 4); /* x3: a milestone */
		g.ev = TR_EV_PASS | TR_EV_NEAR, g.ev_lane = 2;
		tr_react_step(&r, &g, 3);
		assert(r.kind == TR_REACT_COMBO); /* lower than a fresh combo */
		for (int i = 0; i < 40; i++) {
			tr_react_frame(&r);
		}
		tr_react_step(&r, &g, 3);
		assert(r.kind == TR_REACT_NEAR && r.side == 1);
		g.ev = TR_EV_PICKUP; /* combo still x3: not a new milestone */
		tr_react_step(&r, &g, 3);
		assert(r.kind == TR_REACT_NEAR);
		for (int i = 0; i < 100000; i++) {
			tr_react_frame(&r);
		}
		assert(tr_react_ms(&r) == 65535u);
		/* Booth combos (score.h x2..x5): the milestones are x3 and the
		 * top multiplier, x5 -- x4 is a plain pickup, and a pickup that
		 * stays at x5 is no new milestone. */
		g.ev = TR_EV_PICKUP;
		tr_react_step(&r, &g, 4);
		assert(r.kind == TR_REACT_PICKUP);
		tr_react_step(&r, &g, TR_COMBO_MAX);
		assert(TR_COMBO_MAX == 5u && r.kind == TR_REACT_COMBO);
		for (int i = 0; i < 40; i++) {
			tr_react_frame(&r);
		}
		tr_react_step(&r, &g, TR_COMBO_MAX);
		assert(r.kind == TR_REACT_PICKUP);
	}

	/* 4. Deterministic: a whole attract-like run replayed gives the same
	 * reaction stream (kind, side, seq, ms) frame by frame, twice. */
	{
		uint32_t crc[2] = { 0, 0 };

		for (int pass = 0; pass < 2; pass++) {
			tr_game_t  gg;
			tr_score_t sc;
			tr_react_t r;
			uint32_t   x = 99u;

			tr_game_init(&gg, 4242u);
			tr_score_init(&sc);
			tr_react_init(&r);
			for (int f = 0; f < 4000 && gg.alive; f++) {
				tr_intent_t in = none();

				x             = x * 1664525u + 1013904223u;
				in.jump       = (x >> 24) % 9u == 0u;
				in.duck       = (x >> 24) % 9u == 1u;
				in.lane_delta = (int8_t)((x >> 20) % 13u == 0u   ? 1
				                         : (x >> 20) % 13u == 1u ? -1
				                                                 : 0);
				tr_game_step(&gg, in, H);
				tr_score_step(&sc, &gg);
				tr_react_step(&r, &gg, sc.combo);
				tr_react_frame(&r);
				crc[pass] =
				    crc[pass] * 31u + r.kind * 7u + (uint8_t)r.side * 3u + r.seq + tr_react_ms(&r);
			}
		}
		printf("react: replay crc %08x\n", (unsigned)crc[0]);
		assert(crc[0] == crc[1] && crc[0] != 0u);
	}

	/* 5. The packet (tr_mbox.h P16 fields, TR_FLAG_CHAR / TR_FLAG_IDLE). */
	{
		tr_frame_in_t in;
		tr_react_t    r;

		tr_game_init(&g, 1u);
		tr_frame_in_from_game(&in, &g, TR_BANNER_NONE, false, false);
		assert(!(in.flags & (TR_FLAG_CHAR | TR_FLAG_IDLE)) && in.character == 0 && in.react == 0);
		tr_react_init(&r);
		r.kind = TR_REACT_NEAR, r.side = -1, r.seq = 9, r.us = 250000u;
		tr_frame_in_p16(&in, TR_CHAR_FLUX, &r, false, 0u);
		assert((in.flags & TR_FLAG_CHAR) && !(in.flags & TR_FLAG_IDLE));
		assert(in.character == TR_CHAR_FLUX && in.react == TR_REACT_NEAR &&
		       (int8_t)in.react_side == -1);
		assert(in.react_seq == 9 && in.react_ms == 250 && in.idle_ms == 0);
		tr_frame_in_p16(&in, 200u, &r, true, 70000000u);
		assert(in.character == TR_CHAR_PROBE); /* out of range: Probe */
		assert((in.flags & TR_FLAG_IDLE) && in.idle_ms == 65535u);
	}
	printf("react: pass/near/dodge events, priority, clock, replay, packet ok\n");
	return 0;
}
