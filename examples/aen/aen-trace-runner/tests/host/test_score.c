/* tests/host/test_score.c -- player-facing points (src/game/score.c) and the
 * per-tick events tr_game_step() reports for it (state.h TR_EV_*). */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/score.h"

#define TRACK_H 1280

/* One entity alone on the track, parked so it reaches the runner line on
 * the next step (step.c: the runner band is [ry, ry + TR_SCROLL_PX)). */
static void place(tr_game_t *g, tr_entity_kind_t kind, uint8_t lane)
{
	memset(g->ents, 0, sizeof(g->ents));
	g->ents[0] = (tr_entity_t){ .kind = kind, .lane = lane, .y = (int16_t)(tr_runner_ground_y(TRACK_H) - 1) };
}

static void step(tr_game_t *g, tr_score_t *s)
{
	tr_game_step(g, tr_intent_none(), TRACK_H);
	tr_score_step(s, g);
}

int main(void)
{
	tr_game_t  g;
	tr_score_t s;

	/* 1. Events: a collected pickup, a pickup that passes in another lane
	 * (missed), a crash. Exactly one tick carries each. */
	tr_game_init(&g, 5u);
	assert(g.ev == 0u);
	place(&g, TR_ENT_PICKUP, 1);
	tr_game_step(&g, tr_intent_none(), TRACK_H);
	assert(g.ev == TR_EV_PICKUP);
	tr_game_step(&g, tr_intent_none(), TRACK_H);
	assert(g.ev == 0u);
	place(&g, TR_ENT_PICKUP, 0);
	tr_game_step(&g, tr_intent_none(), TRACK_H);
	assert(g.ev == TR_EV_MISS);
	tr_game_step(&g, tr_intent_none(), TRACK_H);
	assert(g.ev == 0u); /* judged once: the pickup has left the runner band */
	place(&g, TR_ENT_OBSTACLE, 1);
	g.ents[0].low = true;
	tr_game_step(&g, tr_intent_none(), TRACK_H);
	assert(!g.alive && g.ev == TR_EV_CRASH);
	tr_game_step(&g, tr_intent_none(), TRACK_H);
	assert(g.ev == TR_EV_CRASH); /* a dead run does not step: the event stays readable */
	tr_game_init(&g, 5u);
	assert(g.ev == 0u);

	/* 2. Distance points: one per metre, a metre TR_PX_PER_M px of track. */
	tr_score_init(&s);
	tr_game_init(&g, 5u);
	memset(g.ents, 0, sizeof(g.ents));
	for (int k = 0; k < 20; k++) {
		g.tick++; /* tick without spawning anything */
		tr_score_step(&s, &g);
	}
	assert(s.metres == 20u * TR_SCROLL_PX / TR_PX_PER_M && s.metres == 3u && s.score == s.metres && s.combo == 0u);

	/* 3. Pickups: +10 x combo, the combo growing with each consecutive
	 * pickup up to TR_COMBO_MAX; a popup per pickup. */
	tr_score_init(&s);
	tr_game_init(&g, 5u);
	uint32_t pts = 0;

	for (unsigned k = 1; k <= TR_COMBO_MAX + 2u; k++) {
		unsigned mult = k < TR_COMBO_MAX ? k : TR_COMBO_MAX;

		place(&g, TR_ENT_PICKUP, 1);
		step(&g, &s);
		pts += 10u * mult;
		assert(s.combo == mult && s.popup_mult == mult && s.popup_pts == 10u * mult);
		assert(s.popup_seq == k);
		assert(s.score == s.metres + pts);
	}

	/* 3b. Booth: the multiplier tops out at x5 (x2..x5 on the HUD), and
	 * the popup always shows the multiplier its points were paid at. */
	assert(TR_COMBO_MAX == 5u && s.combo == 5u && s.popup_mult == 5u && s.popup_pts == 50u);

	/* 3c. The combo window: the next pickup within TR_COMBO_WINDOW steps
	 * of the last keeps the streak; one step later it starts over at x1.
	 * Steps, not frames: the same stretch of track at any pace or ramp. */
	{
		tr_score_t w;
		tr_game_t  wg;

		tr_score_init(&w);
		tr_game_init(&wg, 5u);
		for (uint32_t gap = TR_COMBO_WINDOW; gap <= TR_COMBO_WINDOW + 1u; gap++) {
			place(&wg, TR_ENT_PICKUP, 1);
			step(&wg, &w);
			place(&wg, TR_ENT_PICKUP, 1);
			step(&wg, &w);
			assert(w.combo == 2u);
			memset(wg.ents, 0, sizeof(wg.ents));
			for (uint32_t k = 0; k + 1u < gap; k++) {
				wg.ev = 0u;
				wg.tick++;
				tr_score_step(&w, &wg);
			}
			/* the streak shows while a pickup can still continue it:
			 * W steps on, only a pickup on this very step could */
			assert(w.combo == (gap == TR_COMBO_WINDOW ? 2u : 0u));
			place(&wg, TR_ENT_PICKUP, 1);
			step(&wg, &w); /* `gap` steps after the last pickup */
			assert(gap == TR_COMBO_WINDOW ? (w.combo == 3u && w.popup_mult == 3u)
						      : (w.combo == 1u && w.popup_mult == 1u && w.popup_pts == 10u));
			memset(wg.ents, 0, sizeof(wg.ents));
			for (uint32_t k = 0; k < TR_COMBO_WINDOW + 2u; k++) {
				wg.ev = 0u;
				wg.tick++;
				tr_score_step(&w, &wg);
			}
			assert(w.combo == 0u); /* lapsed: the HUD drops the xN */
		}
		assert(TR_COMBO_WINDOW >= 2u * TR_SPAWN_TICKS); /* room for a pickup two spawns later */
	}

	/* 4. A missed pickup resets the combo; the next pickup is x1 again. */
	place(&g, TR_ENT_PICKUP, 2);
	step(&g, &s);
	assert(s.combo == 0u && s.score == s.metres + pts);
	place(&g, TR_ENT_PICKUP, 1);
	step(&g, &s);
	pts += 10u;
	assert(s.combo == 1u && s.popup_pts == 10u && s.score == s.metres + pts);

	/* 5. A crash resets the combo; the run's score freezes. */
	place(&g, TR_ENT_OBSTACLE, 1);
	g.ents[0].low = true;
	step(&g, &s);
	assert(!g.alive && s.combo == 0u);
	uint32_t final = s.score;

	step(&g, &s);
	assert(s.score == final);

	/* 6. Session best: committed at run end, survives the next run start,
	 * attract (demo) runs never count. */
	assert(s.best == 0u);
	tr_score_run_end(&s, true);
	assert(s.best == final && s.new_best);
	tr_score_run_start(&s);
	assert(s.score == 0u && s.metres == 0u && s.combo == 0u && s.best == final && !s.new_best);
	s.score = final + 100u; /* a better run, but a demo one */
	tr_score_run_end(&s, false);
	assert(s.best == final && !s.new_best);
	tr_score_run_start(&s);
	s.score = final - 1u;
	tr_score_run_end(&s, true);
	assert(s.best == final && !s.new_best);
	tr_score_run_start(&s);
	assert(tr_score_best_now(&s) == final);
	s.score = final + 7u; /* the live BEST readout tracks a run beating it */
	assert(tr_score_best_now(&s) == final + 7u);

	/* 6b. The new-high-score popup: with a table top to beat (hs_top, set
	 * by the caller at run start), the step the run passes it pops one
	 * celebration -- once a run; none with no table (hs_top 0). */
	{
		tr_score_t h;
		tr_game_t  hg;

		tr_score_init(&h);
		tr_game_init(&hg, 5u);
		memset(hg.ents, 0, sizeof(hg.ents));
		h.hs_top = 3u;
		for (int k = 0; k < 60; k++) {
			uint32_t seq = h.popup_seq, was = h.score;

			hg.ev = 0u;
			hg.tick++;
			tr_score_step(&h, &hg);
			assert(h.popup_seq == seq + (was <= 3u && h.score > 3u));
			if (h.popup_seq != seq) {
				assert(h.popup_hs == 1u && h.popup_pts == 0u);
			}
		}
		assert(h.popup_seq == 1u && h.score > 3u);
		/* a pickup's step that passes the top: the pickup's popup shows,
		 * the celebration waits TR_HS_POPUP_GAP steps, then pops */
		{
			tr_score_t q;
			tr_game_t  qg;
			uint32_t   at = 0;

			tr_score_init(&q);
			tr_game_init(&qg, 5u);
			q.hs_top = 5u;
			for (int k = 0; k < 30; k++) { /* metres to just under the top */
				memset(qg.ents, 0, sizeof(qg.ents));
				qg.ev = 0u;
				qg.tick++;
				tr_score_step(&q, &qg);
			}
			assert(q.score <= 5u && q.popup_seq == 0u);
			place(&qg, TR_ENT_PICKUP, 1);
			step(&qg, &q);
			assert(q.score > 5u && q.popup_seq == 1u && q.popup_hs == 0u && q.popup_pts == 10u);
			at = qg.tick;
			for (int k = 0; k < 40; k++) {
				memset(qg.ents, 0, sizeof(qg.ents));
				qg.ev = 0u;
				qg.tick++;
				tr_score_step(&q, &qg);
				if (q.popup_seq == 2u) {
					break;
				}
			}
			assert(q.popup_hs == 1u && qg.tick - at == TR_HS_POPUP_GAP);
		}
		place(&hg, TR_ENT_PICKUP, 1);
		step(&hg, &h);
		assert(h.popup_seq == 2u && h.popup_hs == 0u && h.popup_pts == 10u); /* a pickup's popup again */
		tr_score_run_start(&h);
		assert(h.hs_top == 0u); /* the caller sets it per run */
		for (int k = 0; k < 60; k++) {
			hg.ev = 0u;
			hg.tick++;
			tr_score_step(&h, &hg);
		}
		assert(h.popup_seq == 2u);
	}

	/* 7. Deterministic: two identical runs score identically. */
	tr_game_t  a, b;
	tr_score_t sa, sb;

	tr_game_init(&a, 42u);
	tr_game_init(&b, 42u);
	tr_score_init(&sa);
	tr_score_init(&sb);
	for (int k = 0; k < 500; k++) {
		tr_intent_t in = tr_intent_none();

		in.lane_delta = (int8_t)((k / 7) % 3 - 1);
		tr_game_step(&a, in, TRACK_H);
		tr_score_step(&sa, &a);
		tr_game_step(&b, in, TRACK_H);
		tr_score_step(&sb, &b);
	}
	assert(memcmp(&sa, &sb, sizeof(sa)) == 0 && sa.score > 0u);
	printf("score: run of 500 ticks -> %u pts, %u m, %u pickups\n", (unsigned)sa.score, (unsigned)sa.metres,
	       (unsigned)sa.popup_seq);
	return 0;
}
