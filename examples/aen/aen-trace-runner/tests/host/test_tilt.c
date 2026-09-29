/* tests/host/test_tilt.c -- tilt gesture decoding and the camera-absent booth loop (game/tilt.c). */
#include <assert.h>

#include "../../src/game/tilt.h"
#include "../../src/ipc/tr_mbox.h" /* TR_CHAR_* */

static uint32_t s_rng = 12345u;

/* Uniform noise in [-amp, +amp] Q8. */
static int16_t noise(int amp)
{
	s_rng = s_rng * 1664525u + 1013904223u;
	return (int16_t)((int)((s_rng >> 8) % (uint32_t)(2 * amp + 1)) - amp);
}

/* Feed n identical samples through the booth loop; returns the last event
 * that was not TR_TILT_STAY (or STAY). */
static tr_tilt_event_t feed(tr_tilt_t *t, int n, int16_t x, int16_t y)
{
	tr_tilt_event_t last = TR_TILT_STAY;
	tr_intent_t     in;

	for (int i = 0; i < n; i++) {
		tr_tilt_event_t ev = tr_tilt_step(t, x, y, &in);

		if (ev != TR_TILT_STAY) {
			last = ev;
		}
	}
	return last;
}

#if TR_TILT_TAKEOVER
/* Level, then the deliberate gesture, then back to level: a playing run. */
static void engage(tr_tilt_t *t)
{
	(void)feed(t, 5, 0, 0);
	assert(feed(t, TR_TILT_ENGAGE_TICKS, 120, 0) == TR_TILT_START_RUN);
	assert(t->playing);
	(void)feed(t, 1, 0, 0);
}

#endif

int main(void)
{
	/* 1. A board sitting on its mount never leaves attract: sensor noise
	 * (+-10 Q8 = +-40 mg, many times the BMI323's real noise), a small
	 * steady lean (60 Q8, ~13.5 deg) and isolated knocks (a 3-sample 200 Q8
	 * spike every 50 ticks) over ten minutes of ticks. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		for (int tick = 0; tick < 18000; tick++) {
			int16_t x = noise(10), y = noise(10);

			if (tick >= 6000 && tick < 12000) {
				x = (int16_t)(x + 60);
			}
			if (tick % 50 < 3) {
				x = 200;
			}
			tr_intent_t in;

			assert(tr_tilt_step(&t, x, y, &in) == TR_TILT_STAY);
			assert(!t.playing);
		}
		assert(t.engages == 0u);
	}

#if TR_TILT_TAKEOVER
	/* 2. The deliberate gesture engages -- on the ENGAGE_TICKS-th held
	 * sample, not one sooner -- on either axis, even with noise on top;
	 * the engage tick emits no gesture into the fresh run. */
	for (int axis = 0; axis < 2; axis++) {
		tr_tilt_t   t;
		tr_intent_t in;

		tr_tilt_init(&t);
		(void)feed(&t, 5, 0, 0);
		for (int i = 0; i < TR_TILT_ENGAGE_TICKS; i++) {
			int16_t v  = (int16_t)(-110 + noise(10));
			int16_t x  = axis ? noise(5) : v;
			int16_t y  = axis ? v : noise(5);
			int     ev = tr_tilt_step(&t, x, y, &in);

			assert(ev == ((i + 1 == TR_TILT_ENGAGE_TICKS) ? TR_TILT_START_RUN : TR_TILT_STAY));
			assert(in.lane_delta == 0 && !in.jump && !in.duck);
		}
		assert(t.playing && t.engages == 1u);
		/* Still holding the engage tilt: latched, no phantom lane change. */
		assert(feed(&t, 10, (int16_t)(axis ? 0 : -110), (int16_t)(axis ? -110 : 0)) == TR_TILT_STAY);
		assert(t.gestures == 0u);
	}

	/* 3. A tilt already held when attract starts (board left leaning) does
	 * not engage until it has come back to level first. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		assert(feed(&t, 200, 120, 0) == TR_TILT_STAY);
		assert(!t.playing);
		(void)feed(&t, 1, 0, 0);
		assert(feed(&t, TR_TILT_ENGAGE_TICKS, 120, 0) == TR_TILT_START_RUN);
	}

#else
	/* 2-off. Default build (bench/dev takeover OFF): the deliberate gesture,
	 * repeated, never leaves attract -- pure self-play as at a booth. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		for (int cycle = 0; cycle < 10; cycle++) {
			assert(feed(&t, 5, 0, 0) == TR_TILT_STAY);
			assert(feed(&t, 5 * TR_TILT_ENGAGE_TICKS, 120, 0) == TR_TILT_STAY);
			assert(feed(&t, 5 * TR_TILT_ENGAGE_TICKS, 0, -120) == TR_TILT_STAY);
		}
		assert(!t.playing && t.engages == 0u);
	}
#endif

	/* 4. Lane mapping with hysteresis: a hand hovering across the edge
	 * (85..95) changes lane once; dipping only to 20 (outside the dead zone)
	 * does not re-arm; back to level re-arms; sign maps left/right. */
	{
		tr_tilt_t t;
		int       sum = 0, changes = 0;

		tr_tilt_init(&t);
		for (int i = 0; i < 40; i++) {
			tr_intent_t in = tr_tilt_intent(&t, (int16_t)((i & 1) ? 85 : 95), 0);

			assert(in.source == TR_INPUT_TILT);
			sum += in.lane_delta;
			changes += (in.lane_delta != 0);
		}
		/* 2026W36-0009's mounting (tilt.h TR_TILT_STEER_SIGN -1): x positive
		 * is a tilt to the LEFT, the runner moves left. */
		assert(TR_TILT_STEER_SIGN == -1);
		assert(changes == 1 && sum == -1);
		assert(tr_tilt_intent(&t, 20, 0).lane_delta == 0);
		assert(tr_tilt_intent(&t, 95, 0).lane_delta == 0);
		assert(tr_tilt_intent(&t, 0, 0).lane_delta == 0);
		assert(tr_tilt_intent(&t, -95, 0).lane_delta == 1); /* tilt right: right */
		assert(tr_tilt_intent(&t, noise(10), noise(10)).lane_delta == 0);
		/* Pitch: past the edge up = jump, down = duck, same hysteresis. */
		assert(tr_tilt_intent(&t, 0, 95).jump);
		assert(!tr_tilt_intent(&t, 0, 95).jump);
		assert(tr_tilt_intent(&t, 0, 0).duck == false);
		assert(tr_tilt_intent(&t, 0, -95).duck);
		assert(t.gestures == 4u);
	}

#if TR_TILT_TAKEOVER
	/* 5. Walk-away: IDLE_TICKS without a gesture returns to attract -- not a
	 * tick sooner -- and any gesture restarts the countdown. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		engage(&t);
		assert(feed(&t, TR_TILT_IDLE_TICKS - 10, 0, 0) == TR_TILT_STAY);
		(void)feed(&t, 1, 100, 0); /* a lane change resets the idle count */
		assert(feed(&t, TR_TILT_IDLE_TICKS - 1, 0, 0) == TR_TILT_STAY);
		assert(t.playing);
		assert(feed(&t, 1, 0, 0) == TR_TILT_BACK_TO_ATTRACT);
		assert(!t.playing && t.walkaways == 1u);
	}

	/* 6. Game over: idle player -> attract; active player -> a new run. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		engage(&t);
		(void)feed(&t, 1, 100, 0);
		(void)feed(&t, 1, 0, 0);
		assert(!tr_tilt_run_over(&t));
		assert(t.playing);
		(void)feed(&t, TR_TILT_OVER_IDLE_TICKS, 0, 0);
		assert(tr_tilt_run_over(&t));
		assert(!t.playing && t.walkaways == 1u);
	}

	/* 7. Attract re-engages by itself, cycle after cycle -- no latching,
	 * and the counters the bench reads track every transition. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		for (int cycle = 1; cycle <= 4; cycle++) {
			engage(&t);
			assert(feed(&t, TR_TILT_IDLE_TICKS, 0, 0) == TR_TILT_BACK_TO_ATTRACT);
			assert(t.engages == (uint32_t)cycle && t.walkaways == (uint32_t)cycle);
			assert(feed(&t, 500, noise(10), noise(10)) == TR_TILT_STAY);
			assert(!t.playing);
		}
	}

#endif

	/* 8. P3c sensitivity: a steer fires from 48 Q8 (~10.8 deg), not 47;
	 * re-arms under 20; 2026W36-0009's rest noise (spikes to 18 Q8, both axes)
	 * never fires anything -- and, in a takeover build, never starts a
	 * game, nor does a steer-sized tilt (under ENGAGE 64) held 0.5 s. */
	{
		tr_tilt_t t;

		assert(TR_TILT_EDGE_Q8 == 48 && TR_TILT_DEAD_Q8 == 20 && TR_TILT_ENGAGE_Q8 == 64);
		tr_tilt_init(&t);
		assert(tr_tilt_intent(&t, 47, 0).lane_delta == 0);
		assert(tr_tilt_intent(&t, -47, 0).lane_delta == 0);
		assert(tr_tilt_intent(&t, 48, 0).lane_delta == -1); /* 2026W36-0009: + is a left tilt */
		assert(tr_tilt_intent(&t, 20, 0).lane_delta == 0);  /* not re-armed at 20 */
		assert(tr_tilt_intent(&t, 60, 0).lane_delta == 0);
		assert(tr_tilt_intent(&t, 19, 0).lane_delta == 0);  /* re-armed under 20 */
		assert(tr_tilt_intent(&t, 48, 0).lane_delta == -1);
		tr_tilt_init(&t);
		assert(!tr_tilt_intent(&t, 0, 47).jump && tr_tilt_intent(&t, 0, 48).jump);
		/* a pitch counts only while it dominates the roll: a hard sideways
		 * steer on a vertical panel leaks 256 (1 - cos) into y (a 40 deg
		 * roll: x 165, y 60) and must not jump; the same y alone does */
		tr_tilt_init(&t);
		assert(!tr_tilt_intent(&t, 165, 60).jump);
		assert(!tr_tilt_intent(&t, 0, 0).jump);
		assert(tr_tilt_intent(&t, 10, 60).jump);

		tr_tilt_init(&t);
		for (int i = 0; i < 20000; i++) {
			tr_intent_t in = tr_tilt_intent(&t, noise(18), noise(18));

			assert(in.lane_delta == 0 && !in.jump && !in.duck);
		}
#if TR_TILT_TAKEOVER
		tr_tilt_init(&t);
		assert(feed(&t, 20000, 0, 0) == TR_TILT_STAY);
		for (int i = 0; i < 20000; i++) {
			tr_intent_t out;

			assert(tr_tilt_step(&t, noise(18), noise(18), &out) == TR_TILT_STAY && !t.playing);
		}
		assert(feed(&t, 50, 0, 0) == TR_TILT_STAY);
		assert(feed(&t, TR_TILT_ENGAGE_TICKS * 3, 60, 0) == TR_TILT_STAY && !t.playing); /* a steer, not a takeover */
#endif
	}

	/* 6. Character select on the attract screen (P16): a flick left or
	 * right (past EDGE, back inside DEAD) picks the previous / next
	 * character -- the tilt direction as the steering reads it (x positive
	 * = left on 2026W36-0009, TR_TILT_STEER_SIGN), wrapping; noise, a pitch and a
	 * lean under EDGE pick nothing; a held tilt that takes over starts the
	 * run with the character as it was, and picking stops while playing.
	 * The pick persists across runs and walk-aways (tr_tilt_init at boot
	 * only). The exhibition build (no takeover) never picks: Probe. */
	{
		tr_tilt_t t;
		int16_t   right = (int16_t)(100 * TR_TILT_STEER_SIGN); /* x of a tilt to the right (lane +1) */

		tr_tilt_init(&t);
		assert(t.character == TR_CHAR_PROBE);
		(void)feed(&t, 5, 0, 0);
		(void)feed(&t, 6, right, 0);
		(void)feed(&t, 3, 0, 0);
#if TR_TILT_TAKEOVER
		assert(t.character == 1 && t.picks == 1); /* Probe -> Solder */
		(void)feed(&t, 6, (int16_t)-right, 0);
		(void)feed(&t, 1, 0, 0);
		assert(t.character == 0);
		(void)feed(&t, 6, (int16_t)-right, 0);
		(void)feed(&t, 1, 0, 0);
		assert(t.character == TR_CHAR_N - 1); /* wraps: Pixel */
		(void)feed(&t, 4, right, 0);
		assert(t.character == TR_CHAR_N - 1); /* not until back to level */
		(void)feed(&t, 1, 0, 0);
		assert(t.character == 0);
		for (int i = 0; i < 20000; i++) {
			tr_intent_t out;

			(void)tr_tilt_step(&t, noise(18), noise(18), &out);
		}
		(void)feed(&t, 8, 0, 120); /* a pitch */
		(void)feed(&t, 1, 0, 0);
		(void)feed(&t, 8, 40, 0); /* a lean under EDGE */
		(void)feed(&t, 1, 0, 0);
		assert(t.character == 0 && t.picks == 4 && !t.playing);
		(void)feed(&t, 6, right, 0);
		(void)feed(&t, 1, 0, 0); /* Solder */
		/* the takeover: a held tilt right -- the run starts as Solder */
		assert(feed(&t, TR_TILT_ENGAGE_TICKS, right, 0) == TR_TILT_START_RUN && t.playing);
		(void)feed(&t, 1, 0, 0);
		assert(t.character == 1 && t.picks == 5);
		(void)feed(&t, 6, right, 0); /* playing: a steer, not a pick */
		(void)feed(&t, 1, 0, 0);
		assert(t.character == 1);
		assert(feed(&t, TR_TILT_IDLE_TICKS, 0, 0) == TR_TILT_BACK_TO_ATTRACT);
		assert(t.character == 1); /* kept after the walk-away */
		/* a board still leaning after the walk-away picks nothing: the pick
		 * re-arms only once the board has been level */
		(void)feed(&t, 1, 0, 0);
		assert(feed(&t, TR_TILT_ENGAGE_TICKS, right, 0) == TR_TILT_START_RUN);
		assert(feed(&t, TR_TILT_IDLE_TICKS + 30, right, 0) == TR_TILT_BACK_TO_ATTRACT);
		(void)feed(&t, 5, 0, 0);
		assert(t.character == 1 && t.picks == 5);
#else
		assert(t.character == TR_CHAR_PROBE && t.picks == 0);
#endif
	}

	/* 7. The attract demo shows every character (P16, ALL builds): each
	 * demo run over moves to the next one; a pick pins the choice for the
	 * next player run (no cycling it away), a walk-away unpins it. */
	{
		tr_tilt_t t;

		tr_tilt_init(&t);
		for (int r = 0; r < 2 * (int)TR_CHAR_N; r++) {
			assert(t.character == r % (int)TR_CHAR_N);
			tr_tilt_demo_over(&t);
		}
#if TR_TILT_TAKEOVER
		int16_t right = (int16_t)(100 * TR_TILT_STEER_SIGN);

		(void)feed(&t, 5, 0, 0);
		(void)feed(&t, 6, right, 0);
		(void)feed(&t, 1, 0, 0);
		assert(t.character == 1 && t.pinned);
		tr_tilt_demo_over(&t);
		tr_tilt_demo_over(&t);
		assert(t.character == 1); /* pinned: the next player run is Solder */
		assert(feed(&t, TR_TILT_ENGAGE_TICKS, 0, 120) == TR_TILT_START_RUN && t.character == 1);
		assert(feed(&t, TR_TILT_IDLE_TICKS, 0, 0) == TR_TILT_BACK_TO_ATTRACT && !t.pinned);
		tr_tilt_demo_over(&t);
		assert(t.character == 2); /* the demo cycles on from there */
#endif
	}

	return 0;
}
