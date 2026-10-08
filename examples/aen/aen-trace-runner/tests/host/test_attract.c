/* tests/host/test_attract.c */
#include <assert.h>
#include <stddef.h>

#include "../../src/game/attract.h"
#include "../../src/game/mode.h"
#include "../../src/game/state.h"

#define TRACK_H 1280 /* arbitrary; only tr_attract_intent()'s tr_runner_ground_y() consults it. */

int main(void)
{
	/* 1. No detection for the threshold period enters attract mode -- and
	 * not a tick sooner. */
	{
		tr_attract_t a;
		tr_track_t   t;

		tr_attract_init(&a);
		tr_track_init(&t, 400);

		for (uint32_t k = 0; k < TR_ATTRACT_ENTER_TICKS - 1; k++) {
			assert(tr_attract_step(&a, &t, /*player_present=*/false) == TR_ATTRACT_STAY);
			assert(!a.active);
		}
		assert(tr_attract_step(&a, &t, false) == TR_ATTRACT_ENTERED); /* the caller ends the run */
		assert(a.active);
	}

	/* 2. A player present for TR_ATTRACT_JOIN_TICKS (the join lobby)
	 * leaves attract mode and tells the caller to start a fresh run (the
	 * return value) -- not a tick sooner. */
	{
		tr_attract_t a;
		tr_track_t   t;

		tr_attract_init(&a);
		tr_track_init(&t, 400);

		for (uint32_t k = 0; k < TR_ATTRACT_ENTER_TICKS; k++) {
			(void)tr_attract_step(&a, &t, false);
		}
		assert(a.active);

		for (uint32_t k = 0; k < TR_ATTRACT_JOIN_TICKS - 1; k++) {
			assert(tr_attract_step(&a, &t, /*player_present=*/true) == TR_ATTRACT_STAY);
			assert(a.active && tr_attract_joining(&a));
		}
		assert(tr_attract_step(&a, &t, true) == TR_ATTRACT_LEFT);
		assert(!a.active && !tr_attract_joining(&a));
	}

	/* 3. Leaving attract mode calls tr_track_resync(): the arm edges are
	 * forgotten (the tracker is primed again), so an arm still up from the
	 * lobby does not step a lane in the new run. Entering attract and the
	 * lobby leave the tracker alone. */
	{
		tr_attract_t a;
		tr_track_t   t;
		const int16_t down[2] = { 0, 0 };

		tr_attract_init(&a);
		tr_track_init(&t, 400);
		(void)tr_arms_step(&t.arms, down); /* primed: a pose has been seen */
		assert(t.arms.primed);

		for (uint32_t k = 0; k < TR_ATTRACT_ENTER_TICKS; k++) {
			(void)tr_attract_step(&a, &t, false);
		}
		assert(a.active);
		assert(t.arms.primed); /* entry path never touches the tracker */

		for (uint32_t k = 0; k < TR_ATTRACT_JOIN_TICKS - 1; k++) {
			(void)tr_attract_step(&a, &t, /*player_present=*/true);
			assert(t.arms.primed); /* the lobby leaves the tracker alone */
		}
		assert(tr_attract_step(&a, &t, true) == TR_ATTRACT_LEFT);
		assert(!t.arms.primed); /* resynced */
	}

	/* 4. The synthetic input actually plays: over a run of many ticks the
	 * attract player changes lanes AND clears obstacles (jumps or ducks),
	 * rather than standing still and dying immediately. Runs the real
	 * tr_game_step() so this is an end-to-end check of the intent this
	 * module produces, not just of its bookkeeping. A synthetic run dying
	 * is expected (see TR_ATTRACT_MISS_IN) and is handled the same way
	 * main.c handles it: reinit and keep going -- itself proof attract
	 * "does not latch" on a death.
	 */
	{
		tr_attract_t a;
		tr_game_t    g;

		tr_attract_init(&a);
		tr_game_init(&g, 1u);

		bool lane_changed = false;
		bool cleared      = false; /* out.jump or out.duck fired at least once */
		int  restarts     = 0;

		for (int tick = 0; tick < 600; tick++) {
			tr_intent_t in = tr_attract_intent(&a, &g, TRACK_H);

			if (in.lane_delta != 0) {
				lane_changed = true;
			}
			if (in.jump || in.duck) {
				cleared = true;
			}
			assert(in.source == TR_INPUT_ATTRACT);

			tr_game_step(&g, in, TRACK_H);
			if (!g.alive) {
				restarts++;
				tr_game_init(&g, (uint32_t)(tick + 1)); /* deterministic re-seed, never 0 */
			}
		}
		assert(lane_changed);
		assert(cleared);
		(void)restarts; /* zero or more -- either way the loop above must not have gotten stuck */
	}

	/* 5. Attract mode does not latch: entered, left, and re-entered several
	 * times over without wedging -- each cycle behaves exactly like case 1
	 * and case 2 did in isolation. */
	{
		tr_attract_t a;
		tr_track_t   t;

		tr_attract_init(&a);
		tr_track_init(&t, 400);

		for (int cycle = 0; cycle < 4; cycle++) {
			for (uint32_t k = 0; k < TR_ATTRACT_ENTER_TICKS; k++) {
				tr_attract_ev_t ev = tr_attract_step(&a, &t, false);

				assert(ev ==
				       ((k + 1 < TR_ATTRACT_ENTER_TICKS) ? TR_ATTRACT_STAY : TR_ATTRACT_ENTERED));
				assert((k + 1 < TR_ATTRACT_ENTER_TICKS) ? !a.active : a.active);
			}
			assert(a.active);

			tr_attract_ev_t ev = TR_ATTRACT_STAY;

			for (uint32_t k = 0; k < TR_ATTRACT_JOIN_TICKS; k++) {
				ev = tr_attract_step(&a, &t, true);
			}
			assert(ev == TR_ATTRACT_LEFT);
			assert(!a.active);

			/* One tick of "still present": neither counter may have
			 * survived the cycle. */
			assert(tr_attract_step(&a, &t, true) == TR_ATTRACT_STAY);
			assert(!a.active);
			assert(a.idle_ticks == 0u && a.join_ticks == 0u);
		}
	}

	/* 6. The default build's fallback target is attract, not tilt -- the
	 * property main.c's calibration-timeout and mid-run camera-failure
	 * fall_back() both depend on (see mode.h's TR_FALLBACK_MODE). Compile-
	 * time constant, so this is the whole of what is host-testable about
	 * "reaches attract mode, not tilt, in the default build" without
	 * Zephyr; main.c itself passes TR_FALLBACK_MODE to both call sites
	 * unconditionally (read, not re-verified here).
	 */
	assert(TR_FALLBACK_MODE == TR_MODE_ATTRACT);

	/* 7. P7 pacing: 0.7 x play speed (state.h TR_PLAY_SPEED_Q16; 0.35
	 * steps a frame at the 0.5x game pace) -- exactly TR_ATTRACT_SPEED_Q16
	 * ticks per 65536 frames, evenly (never more frames in a row without a
	 * tick than 1 / speed allows), and the phase the packet carries stays a
	 * Q0.16 fraction. */
	{
		uint32_t phase = 0, ticks = 0;
		int      idle = 0;

		for (uint32_t f = 0; f < 65536u; f++) {
			bool step = tr_attract_pace(&phase);

			assert(phase < 65536u);
			ticks += step;
			idle = step ? 0 : idle + 1;
			assert((uint32_t)idle * TR_ATTRACT_SPEED_Q16 < 65536u);
		}
		assert(ticks == TR_ATTRACT_SPEED_Q16 && phase == 0u);
		assert(TR_ATTRACT_SPEED_Q16 * 10u / 7u + 2u >= TR_PLAY_SPEED_Q16 &&
		       TR_ATTRACT_SPEED_Q16 * 10u / 7u <= TR_PLAY_SPEED_Q16); /* 0.7 of play */
		assert(TR_ATTRACT_WANDER_IN >= 80);                           /* calmer self-play */
	}

	/* 8. P4b: the attract player reacts to a live wire in its lane like to
	 * any obstacle -- jumps a low one, ducks a high one, never the wrong
	 * move -- apart from the deliberate one-in-TR_ATTRACT_MISS_IN miss. */
	for (int low = 0; low <= 1; low++) {
		int right = 0, wrong = 0;

		for (uint32_t seed = 1; seed <= 36; seed++) {
			tr_attract_t a;
			tr_game_t    g;

			tr_attract_init(&a);
			a.rng = seed * 0x9E3779B9u;
			tr_game_init(&g, seed);
			for (unsigned i = 0; i < TR_MAX_ENTITIES; i++) {
				g.ents[i].kind = TR_ENT_FREE;
			}
			g.ents[2] =
			    (tr_entity_t){ .kind = TR_ENT_WIRE,
				               .lane = g.lane,
				               .low  = low,
				               .y    = (int16_t)(tr_runner_ground_y(TRACK_H) - 3 * TR_SCROLL_PX) };

			tr_intent_t in = tr_attract_intent(&a, &g, TRACK_H);

			right += low ? in.jump : in.duck;
			wrong += low ? in.duck : in.jump;
		}
		assert(wrong == 0 && right >= 36 - 36 / TR_ATTRACT_MISS_IN - 4);
	}

	/* 9. The attract lobby (P16): the runner stands idle for
	 * TR_LOBBY_FRAMES on EVERY way into attract -- boot, the camera
	 * attract sub-state, a fall back, a walk-away (all: attract false ->
	 * true) -- and again after each demo run; leaving attract (a player
	 * takes over) ends it at once. idle_us counts the time it has stood. */
	{
		tr_lobby_t l;
		int        standing = 0;

		tr_lobby_init(&l);
		for (int f = 0; f < TR_LOBBY_FRAMES + 5; f++) { /* boot straight into attract */
			bool st = tr_lobby_frame(&l, true);

			assert(st == (f < TR_LOBBY_FRAMES) && l.standing == st);
			if (st) {
				assert(l.idle_us == (uint32_t)f * TR_PANEL_PERIOD_US);
			}
			standing += st;
		}
		assert(standing == TR_LOBBY_FRAMES);
		tr_lobby_demo_over(&l); /* a demo run crashed: the lobby again */
		assert(tr_lobby_frame(&l, true) && l.idle_us == 0u);
		assert(!tr_lobby_frame(&l, false) && !l.standing); /* taken over: at once */
		for (int f = 0; f < 50; f++) {
			assert(!tr_lobby_frame(&l, false)); /* a player run: never */
		}
		assert(tr_lobby_frame(&l, true) &&
		       l.idle_us == 0u); /* back into attract (walk-away, fall back) */
		tr_lobby_init(&l);
		assert(!tr_lobby_frame(&l, false)); /* a vision boot: a player first */
		assert(tr_lobby_frame(&l, true));   /* then the camera attract */
	}

	return 0;
}
