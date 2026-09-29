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

		tr_track_init(&t, 640, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
		tr_ctl_init(&c, TR_MODE_VISION);

		bool run = true;

		for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
			(void)tr_track_update(&t, none());

			run = tr_ctl_step(&c, &t, tr_track_player_lost(&t), 1u);
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

		tr_track_init(&t, 640, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
		tr_ctl_init(&c, TR_MODE_VISION);
		tr_game_init(&g, 1);

		for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
			tr_intent_t in = tr_track_update(&t, none());

			if (tr_ctl_step(&c, &t, tr_track_player_lost(&t), g.lane)) {
				tr_game_step(&g, in, TRACK_H);
			}
		}
		assert(c.paused);
		uint32_t tick_at_pause = g.tick;

		for (int k = 0; k < 5; k++) {
			tr_intent_t in  = tr_track_update(&t, none());
			bool        run = tr_ctl_step(&c, &t, tr_track_player_lost(&t), g.lane);

			assert(!run);
			if (run) {
				tr_game_step(&g, in, TRACK_H);
			}
		}
		assert(g.tick == tick_at_pause); /* frozen for the whole paused stretch */
	}

	/* 3. On the frame the player is re-acquired, the run unpauses AND
	 * tr_track_resync() is called with the game's current lane -- passed
	 * here as 0, deliberately different from anything track_update() itself
	 * would compute, so the assertion can only pass if the resync argument
	 * (not the tracker's own delta) is what lands in t.lane. */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 640, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
		tr_ctl_init(&c, TR_MODE_VISION);
		c.paused = true; /* as if a prior pause was already in effect */

		(void)tr_track_update(&t, box(270, 100, 100, 300, 90)); /* valid: reacquired */
		bool run = tr_ctl_step(&c, &t, tr_track_player_lost(&t), 0u);

		assert(!c.paused);
		assert(!run); /* this tick is spent resyncing, not stepping -- see case 2 */
		assert(t.lane == 0u);
	}

	/*
	 * 4. Regression test for mode.c's pause-exit resync (RESYNC SITE 1/2).
	 *
	 * NOTE (fix round 1): this does NOT reproduce the original plan's
	 * pseudocode failing -- reviewed and confirmed: the plan clears `paused`
	 * and applies that tick's delta on the very same tick the box goes
	 * valid, and an invalid box never touches t.lane (track.c early-returns
	 * before reaching it), so the plan's logic actually stays in sync for
	 * this exact scenario. Case 5 below is the one that fails against the
	 * plan's literal logic. What THIS case pins is a real hazard this
	 * design's own tr_ctl_step() deliberately creates: it returns false (does
	 * not step the game) on the tick pause is left, and tr_track_update()
	 * still mutates t.lane on that same tick regardless. Without the resync
	 * call, that mutation is never corrected and the tracker silently
	 * disagrees with the game from then on.
	 *
	 * tr_track_update() mutates its own t.lane on every call, whether or not
	 * the caller ever applies the result to the game -- and Step 5's loop
	 * shape calls it every tick even while paused (that is the only way to
	 * notice the player came back). So: drive the player to lane 2 for
	 * real, force a pause, then -- while still paused -- call
	 * tr_track_update() directly several more times (bypassing tr_ctl_step,
	 * exactly as the main loop's per-tick camera feed would) with the box
	 * standing somewhere that pulls the tracker's lane belief away from 2.
	 * None of those deltas may ever reach the game (asserted below), and
	 * when the player is genuinely re-acquired, the tracker's lane must
	 * equal the game's lane -- not the lane those ignored deltas would have
	 * left it at, and not even the transition tick's own delta.
	 */
	{
		tr_track_t t;
		tr_ctl_t   c;
		tr_game_t  g;

		tr_track_init(&t, 640, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
		tr_ctl_init(&c, TR_MODE_VISION);
		tr_game_init(&g, 1);
		assert(g.lane == 1u); /* TR_LANES/2 */

		/* Drive to lane 2 for real: box centred at 500 is right of the
		 * right-hand edge (426 for a 640-wide frame), one step right. */
		tr_intent_t in = tr_track_update(&t, box(450, 100, 100, 300, 90));

		assert(tr_ctl_step(&c, &t, tr_track_player_lost(&t), g.lane));
		tr_game_step(&g, in, TRACK_H);
		assert(t.lane == 2u && g.lane == 2u);

		/* Lose the player: pause. Below TR_TRACK_LOST_LIMIT consecutive
		 * misses the game is still legitimately ticking on a no-op intent
		 * (see case 2's comment) -- only c.paused at the end matters here. */
		for (int k = 0; k < TR_TRACK_LOST_LIMIT + 1; k++) {
			in = tr_track_update(&t, none());
			if (tr_ctl_step(&c, &t, tr_track_player_lost(&t), g.lane)) {
				tr_game_step(&g, in, TRACK_H);
			}
		}
		assert(c.paused);
		assert(t.lane == 2u && g.lane == 2u); /* invalid frames never touch t.lane; a no-op
							* intent never moves the lane either */

		/* Several frames' worth of deltas that WOULD move the tracker if
		 * applied -- fed straight to tr_track_update(), as the camera loop
		 * does every tick regardless of pause. A box centred at 40 is left
		 * of the left-hand edge; one lane steps left per call. */
		(void)tr_track_update(&t, box(40, 100, 100, 300, 90)); /* 2 -> 1 */
		(void)tr_track_update(&t, box(40, 100, 100, 300, 90)); /* 1 -> 0 */
		(void)tr_track_update(&t, box(40, 100, 100, 300, 90)); /* stays 0 */
		assert(t.lane == 0u);                                  /* proof the drift really happened */
		assert(g.lane == 2u);                                  /* and the game never moved */

		/* Re-acquire: box back in the centre. */
		in       = tr_track_update(&t, box(270, 100, 100, 300, 90));
		bool run = tr_ctl_step(&c, &t, tr_track_player_lost(&t), g.lane);

		assert(!run);
		assert(!c.paused);
		/* THE assertion: the tracker's lane equals the game's lane (2), not
		 * the drifted 0, and not the 1 this same reacquisition call's own
		 * delta would have produced (0 -> 1) had resync not overridden it.
		 * Measured with mode.c's resync call removed (verification only,
		 * never committed that way): t.lane == 1, g.lane == 2 -- a
		 * permanent, silent one-lane desync from that point on. Removing
		 * the tr_track_resync() call in mode.c's tr_ctl_step() reproduces
		 * exactly that and fails this assertion.
		 */
		assert(t.lane == g.lane);
	}

	/* 5. A run reset calls tr_track_resync() with the reset lane -- THIS is
	 * the case that fails against the plan's own literal pause logic: the
	 * plan never resyncs on a run reset at all. tr_game_init() snaps g.lane
	 * back to TR_LANES/2 while t.lane still holds wherever the previous
	 * run's player last stood (or died), and nothing in the plan's pause
	 * logic -- which only ever looks at tr_track_player_lost() and
	 * in.source -- has any path that corrects it. Confirmed by removing
	 * mode.c's tr_ctl_reset() resync call: the suite fails right here. */
	{
		tr_track_t t;
		tr_ctl_t   c;

		tr_track_init(&t, 640, 400);
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
		tr_ctl_init(&c, TR_MODE_VISION);
		c.paused = true;

		/* Drift the tracker away from the reset lane first. */
		(void)tr_track_update(&t, box(450, 100, 100, 300, 90)); /* 1 -> 2 */
		assert(t.lane == 2u);

		tr_ctl_reset(&c, &t, 1u); /* TR_LANES/2, the lane tr_game_init() resets to */
		assert(t.lane == 1u);
		assert(!c.paused);
	}

	/* 6. Tilt mode never consults the tracker and never pauses on
	 * player-lost -- track is passed NULL and player_lost is forced true,
	 * neither of which may change the outcome. */
	{
		tr_ctl_t c;

		tr_ctl_init(&c, TR_MODE_TILT);

		bool run = tr_ctl_step(&c, NULL, /*player_lost=*/true, 0u);

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

		bool run = tr_ctl_step(&c, NULL, /*player_lost=*/true, 0u);

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

		tr_track_init(&t, 640, 400);
		tr_ctl_init(&c, TR_MODE_VISION);
		assert(!t.calibrated);

		for (int k = 0; k < 50; k++) {
			tr_track_calibrate(&t, none(), 640); /* fails every time: box is invalid */
			assert(!t.calibrated);

			(void)tr_track_update(&t, none());
			(void)tr_ctl_step(&c, &t, tr_track_player_lost(&t), 1u); /* must not crash or wedge */
		}

		/* A valid box still calibrates after 50 prior failures. */
		tr_track_calibrate(&t, box(270, 100, 100, 300, 90), 640);
		assert(t.calibrated);

		/* And tracking resumes immediately -- no lingering damage from the
		 * failed attempts. */
		tr_intent_t in = tr_track_update(&t, box(270, 100, 100, 300, 90));

		assert(in.source == TR_INPUT_VISION);
	}

	return 0;
}
