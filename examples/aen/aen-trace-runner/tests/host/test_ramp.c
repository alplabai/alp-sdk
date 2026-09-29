/* tests/host/test_ramp.c -- the in-run difficulty ramp (src/game/ramp.h):
 * the world speed starts at today's pace, grows monotonically with the
 * steps taken, reaches its cap in ~90 s of real play and never passes one
 * game step a frame; the zones keep their real time (zone.h ramp_q8); the
 * attract AI plays the same game at every speed. Built at 40 and 30 Hz. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/attract.h"
#include "../../src/game/ramp.h"
#include "../../src/game/zone.h"

#define TRACK_H 1280

int main(void)
{
	/* 1. The curve: today's pace at the start, never slower a step later,
	 * capped at TR_RAMP_MAX_Q8 from TR_RAMP_STEPS on. */
	{
		assert(tr_ramp_q8(0u) == 256u && tr_ramp_speed_q16(TR_PLAY_SPEED_Q16, 0u) == TR_PLAY_SPEED_Q16);
		assert(tr_ramp_speed_q16(TR_ATTRACT_SPEED_Q16, 0u) == TR_ATTRACT_SPEED_Q16);
		for (uint32_t s = 1; s < 3u * TR_RAMP_STEPS; s++) {
			assert(tr_ramp_q8(s) >= tr_ramp_q8(s - 1u) && tr_ramp_q8(s) <= TR_RAMP_MAX_Q8);
			assert(tr_ramp_speed_q16(TR_PLAY_SPEED_Q16, s) >= tr_ramp_speed_q16(TR_PLAY_SPEED_Q16, s - 1u));
			assert(tr_ramp_speed_q16(TR_PLAY_SPEED_Q16, s) <= 65536u); /* never two steps a frame */
		}
		assert(tr_ramp_q8(TR_RAMP_STEPS - 1u) < TR_RAMP_MAX_Q8 && tr_ramp_q8(TR_RAMP_STEPS) == TR_RAMP_MAX_Q8);
		assert(tr_ramp_q8(0xFFFFFFFFu) == TR_RAMP_MAX_Q8); /* no overflow on a very long run */
		/* gently: no step changes the speed by more than a fraction of a percent */
		assert((TR_RAMP_MAX_Q8 - 256u) * 1000u / TR_RAMP_STEPS < 256u);
	}

	/* 2. Real time, frame by frame at TR_PANEL_HZ: the cap arrives in about
	 * 90 s of play; the speed at the cap is 1.5x today's (1.5x is exactly
	 * one step a frame at 30 Hz, the most the loop takes). */
	{
		uint32_t phase = 0, steps = 0, frames = 0;

		while (tr_ramp_q8(steps) < TR_RAMP_MAX_Q8) {
			steps += tr_game_pace(&phase, tr_ramp_speed_q16(TR_PLAY_SPEED_Q16, steps));
			frames++;
		}
		double secs = (double)frames / TR_PANEL_HZ;

		printf("ramp: cap %u/256 after %u steps, %.1f s of play at %u Hz\n", (unsigned)TR_RAMP_MAX_Q8,
		       (unsigned)steps, secs, (unsigned)TR_PANEL_HZ);
		assert(secs > 85.0 && secs < 95.0);
		uint32_t cap = tr_ramp_speed_q16(TR_PLAY_SPEED_Q16, steps);

		assert(cap * 2u >= TR_PLAY_SPEED_Q16 * 3u - 2u && cap <= 65536u);
		/* steps per second at the cap, measured: 30 (1.5 x the 20 of today's pace) */
		uint32_t n = 0;

		for (uint32_t f = 0; f < 60u * TR_PANEL_HZ; f++) {
			n += tr_game_pace(&phase, cap);
		}
		assert(n >= 60u * 30u - 2u && n <= 60u * 30u + 1u);
	}

	/* 3. The zones keep their real time under the ramp -- wired as main.c
	 * does it (the frame speed from tr_ramp_frame_q16(), ramp_q8 from
	 * tr_ramp_stepped_q8() after the step): every entry still comes 40 s
	 * of play apart (TR_ZONE_STEPS_PLAY at today's 20 steps/s), however
	 * fast the world runs by then, the gate on the runner line on the
	 * entry step. Unramped (ramp_q8 0) the schedule is today's. Attract
	 * too, whose zone is tightest (TR_ZONE_STEPS_ATTRACT, 16 s at 14
	 * steps/s): the gate's lead + tail are steps (the ramp speeds the steps,
	 * not the gate per step), each counting 256/ramp of the zone, so the
	 * margin only grows at speed -- every entry still 16 s apart. */
	for (int att = 0; att < 2; att++) {
		tr_zone_t z;
		uint32_t  phase = 0, steps = 0, last = 0, entries = 0;
		double    want  = (att ? TR_ZONE_STEPS_ATTRACT : TR_ZONE_STEPS_PLAY) * 65536.0 /
			      ((double)(att ? TR_ATTRACT_SPEED_Q16 : TR_PLAY_SPEED_Q16) * TR_PANEL_HZ);

		tr_zone_reset(&z);
		for (uint32_t f = 1; entries < (att ? 12u : 6u); f++) { /* past the cap either way */
			if (!tr_game_pace(&phase, tr_ramp_frame_q16(att, steps))) {
				continue;
			}
			steps++; /* the game step: g.tick after it */
			z.ramp_q8 = tr_ramp_stepped_q8(steps);
			if (tr_zone_step(&z, att, TRACK_H)) {
				double gap = (double)(f - last) / TR_PANEL_HZ;

				assert(z.gate_y >= tr_runner_ground_y(TRACK_H));
				printf("ramp: %s zone %u entered after %.2f s (ramp %u/256)\n", att ? "attract" : "play",
				       (unsigned)z.zone, gap, (unsigned)z.ramp_q8);
				assert(gap > want - 0.5 && gap < want + 0.5);
				last = f;
				entries++;
			}
			assert(f < 400u * TR_PANEL_HZ);
		}
		assert(tr_ramp_q8(steps) == TR_RAMP_MAX_Q8); /* the later zones ran at the cap */
		assert(tr_ramp_stepped_q8(0u) == 256u && tr_ramp_stepped_q8(1u) == 256u);
	}

	/* 4. The packet's pace_q8 (tr_mbox.h): the frame speed in Q.8, never
	 * 0 while running -- one step a frame (the cap at 30 Hz) is 255, not
	 * 256 wrapped to the old HE's 0 (the renderer's frame_k, +33 % motion
	 * at 30 Hz). */
	for (int att = 0; att < 2; att++) {
		for (uint32_t st = 0; st <= TR_RAMP_STEPS + 10u; st++) {
			uint32_t sp = tr_ramp_frame_q16(att, st);
			uint8_t  q  = tr_ramp_pace_q8(sp);

			assert(q != 0u && (sp < 65536u ? q == sp >> 8 : q == 255u));
		}
	}
	assert(tr_ramp_pace_q8(65536u) == 255u && tr_ramp_pace_q8(TR_PLAY_SPEED_Q16) == TR_PLAY_SPEED_Q16 >> 8);
	if (TR_PANEL_HZ == 30) {
		assert(tr_ramp_pace_q8(tr_ramp_frame_q16(false, TR_RAMP_STEPS)) == 255u);
	}

	/* 5. The attract AI at every speed: the ramp changes only WHEN the
	 * steps land, never what they do -- a ramped, frame-paced attract
	 * session is step for step an unpaced one run on its own (the same
	 * per-step hash). Over long sessions the AI still clears what it
	 * reacts to at the capped speed: its only deaths are the deliberate
	 * one-in-TR_ATTRACT_MISS_IN misses. */
	{
		uint32_t cleared_at_cap = 0, deaths_at_cap = 0, deaths = 0, clears = 0, longest = 0;

		for (uint32_t seed = 1; seed <= 12u; seed++) {
			uint32_t hash[2] = { 2166136261u, 2166136261u }, nsteps = 0;

			for (int paced = 1; paced >= 0; paced--) {
				tr_attract_t a;
				tr_game_t    g;
				uint32_t     phase = 0, done = 0, restarts = 0;

				tr_attract_init(&a);
				a.rng = seed * 0x9E3779B9u;
				tr_game_init(&g, seed);
				for (uint32_t f = 0; paced ? f < 200000u : done < nsteps; f++) {
					if (paced && !tr_game_pace(&phase, tr_ramp_frame_q16(true, g.tick))) {
						continue;
					}
					bool at_cap = tr_ramp_q8(g.tick) == TR_RAMP_MAX_Q8;

					tr_game_step(&g, tr_attract_intent(&a, &g, TRACK_H), TRACK_H);
					done++;
					hash[paced] = (hash[paced] ^ (g.tick * 31u + g.lane * 7u + g.score * 3u + g.alive)) * 16777619u;
					if (paced) {
						clears += (g.ev & TR_EV_PASS) != 0u;
						cleared_at_cap += at_cap && (g.ev & TR_EV_PASS);
					}
					if (!g.alive) {
						if (paced) {
							deaths++;
							deaths_at_cap += at_cap;
							longest = g.tick > longest ? g.tick : longest;
						}
						tr_game_init(&g, seed + ++restarts);
					}
				}
				if (paced) {
					nsteps = done;
				}
			}
			assert(hash[0] == hash[1]);
		}
		printf("ramp: attract %u clears, %u deaths (%u clears / %u deaths at the cap), longest run %u steps\n",
		       (unsigned)clears, (unsigned)deaths, (unsigned)cleared_at_cap, (unsigned)deaths_at_cap,
		       (unsigned)longest);
		assert(longest >= TR_RAMP_STEPS && cleared_at_cap > 50u);
		/* one miss in TR_ATTRACT_MISS_IN reactions: the deaths stay at
		 * that rate (plus slack), at the cap as over the whole session */
		assert(deaths * (TR_ATTRACT_MISS_IN - 2u) <= clears + deaths);
		assert(deaths_at_cap * (TR_ATTRACT_MISS_IN - 3u) <= cleared_at_cap + deaths_at_cap);
	}
	return 0;
}
