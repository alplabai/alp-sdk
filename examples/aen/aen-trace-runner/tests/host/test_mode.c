/* tests/host/test_mode.c */
#include <assert.h>
#include <stddef.h>

#include "../../src/game/mode.h"
#include "../../src/game/state.h"

#define TRACK_H 1280 /* arbitrary; only tr_game_step()'s spawn() consults it. */

static tr_box_t box(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t conf)
{
	return (tr_box_t){ .x = x, .y = y, .w = w, .h = h, .confidence = conf, .valid = true };
}

/* A torso box with the left and right arm raise levels (arms.h) given. */
static tr_box_t arm_box(int16_t left, int16_t right)
{
	tr_box_t b = box(270, 100, 100, 300, 90);

	b.arm_raise[TR_ARM_LEFT]  = left;
	b.arm_raise[TR_ARM_RIGHT] = right;
	return b;
}

static tr_box_t none(void)
{
	return (tr_box_t){ .valid = false };
}

int main(void)
{
	/* 1. Losing the player pauses the run. */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
		tr_ctl_init(&c, TR_MODE_VISION);

		bool run = true;

		for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
			(void)tr_track_update(&t, none());

			run = tr_ctl_step(&c, &t, tr_track_player_lost(&t));
		}
		assert(c.paused);
		assert(!run);
	}

	/* 2. While paused, tr_game_step() is not called -- the game must not
	 * advance at all across a run of paused ticks.
	 *
	 * Below TR_TRACK_LOST_LIMIT consecutive misses, tr_track_player_lost()
	 * is still false and the game legitimately keeps ticking on a no-op
	 * intent (a brief detection miss is noise, not "the player is gone");
	 * only once the limit is crossed does the pause actually start, so the
	 * frozen-tick assertion below only covers ticks from that point on. */
	{
		tr_track_t t;
		tr_ctl_t   c;
		tr_game_t  g;

		tr_track_init(&t, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
		tr_ctl_init(&c, TR_MODE_VISION);
		tr_game_init(&g, 1);

		for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
			tr_intent_t in = tr_track_update(&t, none());

			if (tr_ctl_step(&c, &t, tr_track_player_lost(&t))) {
				tr_game_step(&g, in, TRACK_H);
			}
		}
		assert(c.paused);
		uint32_t tick_at_pause = g.tick;

		for (int k = 0; k < 5; k++) {
			tr_intent_t in  = tr_track_update(&t, none());
			bool        run = tr_ctl_step(&c, &t, tr_track_player_lost(&t));

			assert(!run);
			if (run) {
				tr_game_step(&g, in, TRACK_H);
			}
		}
		assert(g.tick == tick_at_pause); /* frozen for the whole paused stretch */
	}

	/* 3. On the frame the player is re-acquired, the run unpauses AND
	 * tr_track_resync() forgets the arm edges (the tracker is primed again). */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
		tr_ctl_init(&c, TR_MODE_VISION);
		c.paused = true; /* as if a prior pause was already in effect */

		(void)tr_track_update(&t, box(270, 100, 100, 300, 90)); /* valid: reacquired */
		assert(t.arms.primed);
		bool run = tr_ctl_step(&c, &t, tr_track_player_lost(&t));

		assert(!c.paused);
		assert(!run); /* this tick is spent resyncing, not stepping -- see case 2 */
		assert(!t.arms.primed);
	}

	/*
	 * 4. Regression test for mode.c's pause-exit resync (RESYNC SITE 1/2).
	 * tr_track_update() runs every tick even while paused (that is the only
	 * way to notice the player came back), so an arm raised during the pause
	 * -- or still up as the player walks back in -- is seen by the tracker
	 * and never by the game. When play resumes it must not turn into a lane
	 * step: the arm has to be lowered and raised again first.
	 */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
		tr_ctl_init(&c, TR_MODE_VISION);

		/* Raise the left arm for real: one lane left. */
		tr_intent_t in = tr_track_update(&t, arm_box(0, 0)); /* the priming pose */

		for (int k = 0; k < TR_ARM_SETTLE_POSES; k++) {
			in = tr_track_update(&t, arm_box(80, 0));
		}
		assert(in.lane_delta == -1);
		assert(tr_ctl_step(&c, &t, tr_track_player_lost(&t)));

		/* Lose the player: pause. */
		for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
			(void)tr_track_update(&t, none());
			(void)tr_ctl_step(&c, &t, tr_track_player_lost(&t));
		}
		assert(c.paused);

		/* The player comes back with the left arm still raised: the pause
		 * ends on a spent tick, and the arm stays spent. */
		(void)tr_track_update(&t, arm_box(80, 0));
		bool run = tr_ctl_step(&c, &t, tr_track_player_lost(&t));

		assert(!run && !c.paused);
		for (int k = 0; k < 3 * TR_ARM_SETTLE_POSES; k++) {
			in = tr_track_update(&t, arm_box(80, 0));
			assert(in.lane_delta == 0 && !in.jump);
		}
		/* Lowered, then raised again: an ordinary step. */
		(void)tr_track_update(&t, arm_box(-60, 0));
		for (int k = 0; k < TR_ARM_SETTLE_POSES; k++) {
			in = tr_track_update(&t, arm_box(80, 0));
		}
		assert(in.lane_delta == -1);
	}

	/* 5. A run reset forgets the arm edges too -- a player who restarts with an
	 * arm still up must lower it before it counts, or the new run's first
	 * tick would step a lane on the old gesture. */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
		tr_ctl_init(&c, TR_MODE_VISION);
		c.paused = true;

		(void)tr_track_update(&t, arm_box(80, 0));
		assert(t.arms.primed);

		tr_ctl_reset(&c, &t);
		assert(!t.arms.primed);
		assert(!c.paused);
		for (int k = 0; k < 3 * TR_ARM_SETTLE_POSES; k++) {
			tr_intent_t in = tr_track_update(&t, arm_box(80, 0));

			assert(in.lane_delta == 0);
		}
	}

	/* 6. Tilt mode never consults the tracker and never pauses on
	 * player-lost -- track is passed NULL and player_lost is forced true,
	 * neither of which may change the outcome. */
	{
		tr_ctl_t c;

		tr_ctl_init(&c, TR_MODE_TILT);

		bool run = tr_ctl_step(&c, NULL, /*player_lost=*/true);

		assert(run);
		assert(!c.paused);
	}

	/* 6b. The standalone TR_MODE_ATTRACT fallback (mode.h's TR_FALLBACK_MODE)
	 * behaves exactly like tilt here -- never consults the tracker, never
	 * pauses -- because it has no camera either (see main.c's fall_back()).
	 * Not to be confused with the REVERSIBLE attract sub-state that runs
	 * WITHIN vision mode (game/attract.c's own tr_attract_step()), which
	 * this file never touches. */
	{
		tr_ctl_t c;

		tr_ctl_init(&c, TR_MODE_ATTRACT);

		bool run = tr_ctl_step(&c, NULL, /*player_lost=*/true);

		assert(run);
		assert(!c.paused);
	}

	/*
	 * 7. Vision needs BOTH camera and detector; anything else falls back to
	 *    TR_FALLBACK_MODE -- NOT to a hardcoded TR_MODE_TILT.
	 *
	 *    This case used to assert TR_MODE_TILT, and that assertion is what
	 *    let the bug through: attract mode was unreachable in practice
	 *    because initial selection ignored TR_FALLBACK_MODE, and the test
	 *    agreed with the bug. Found on hardware, not here. Asserting against
	 *    TR_FALLBACK_MODE instead means the test follows the build's own
	 *    choice rather than restating one of its two possible values.
	 */
	{
		assert(tr_ctl_select_mode(false, false) == TR_FALLBACK_MODE);
		assert(tr_ctl_select_mode(true, false) == TR_FALLBACK_MODE);
		assert(tr_ctl_select_mode(false, true) == TR_FALLBACK_MODE);
		assert(tr_ctl_select_mode(true, true) == TR_MODE_VISION);
	}

	/*
	 * 7b. The DEFAULT build must fall back to attract, not tilt.
	 *
	 *     Case 7 alone would pass with TR_FALLBACK_MODE defined as either
	 *     value, so on its own it cannot catch the default being wrong --
	 *     and the default is the whole point: a booth panel is mounted and
	 *     nobody can tilt it. A bench build overriding to TR_MODE_TILT is
	 *     deliberate and excluded here.
	 */
#ifndef TR_FALLBACK_MODE_OVERRIDDEN
	assert(TR_FALLBACK_MODE == TR_MODE_ATTRACT);
	assert(tr_ctl_select_mode(false, false) == TR_MODE_ATTRACT);
#endif

	/*
	 * 8. (fix round 1) The property main.c's calibration-retry loop depends
	 * on: repeated failed tr_track_calibrate() attempts (invalid boxes)
	 * never latch into a state a later valid box cannot still calibrate
	 * from, and until calibration succeeds an uncalibrated tracker only ever
	 * drives tr_ctl_step() into the ordinary pause path -- never a crash,
	 * never anything else. This is what makes "keep the title banner up and
	 * keep retrying every tick until calibrated" (see main.c) a real
	 * recovery and not another dead end: main.c itself is not host-testable
	 * (it is all Zephyr calls), so this pins the track.c/mode.c contract
	 * that loop relies on.
	 */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 400);
		tr_ctl_init(&c, TR_MODE_VISION);
		assert(!t.calibrated);

		for (int k = 0; k < 50; k++) {
			tr_track_calibrate(&t, none()); /* fails every time: box is invalid */
			assert(!t.calibrated);

			(void)tr_track_update(&t, none());
			(void)tr_ctl_step(&c, &t, tr_track_player_lost(&t)); /* must not crash or wedge */
		}

		/* A valid box still calibrates after 50 prior failures. */
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90));
		assert(t.calibrated);

		/* And tracking resumes immediately -- no lingering damage from the
		 * failed attempts. */
		tr_intent_t in = tr_track_update(&t, box(270, 100, 100, 300, 90));

		assert(in.source == TR_INPUT_VISION);
	}

	return 0;
}
