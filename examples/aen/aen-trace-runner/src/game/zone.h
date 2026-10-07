/* src/game/zone.h -- world zones (P15): the run travels through the zones in
 * sequence, one every TR_ZONE_STEPS_* game steps, each boundary a gate that
 * comes down the track like an entity. Pure and header-only: the HE steps a
 * tr_zone_t with the game and ships it in the frame packet (tr_mbox.h
 * TR_FLAG_ZONE, zone + gate_y); the A32 scene draws the gate and blends the
 * look from those two fields alone; the HUD shows the zone's name on entry.
 *
 * Zone 0 is the circuit board the game always ran on (and what a renderer
 * draws for an old HE that never sets TR_FLAG_ZONE); the four the maintainer
 * picked follow it, then the run loops back to the board.
 *
 * A step counter, not a clock: the zone schedule is a pure function of the
 * game steps taken since tr_zone_reset(), so the same run always meets its
 * gates at the same places. Distance per step is fixed (state.h
 * TR_SCROLL_PX), so this is distance travelled.
 */
#ifndef TR_ZONE_H
#define TR_ZONE_H

#include <stdbool.h>
#include <stdint.h>

#include "state.h"

#define TR_ZONES 5

#define TR_ZONE_BOARD 0u /* the PCB at dusk (P2..P12) */
#define TR_ZONE_DIE   1u /* CPU die city */
#define TR_ZONE_MEM   2u /* memory canyon */
#define TR_ZONE_RF    3u /* antenna / RF field */
#define TR_ZONE_NEON  4u /* night / neon city */

/* Game steps from entering a zone to entering the next one. Play runs 20
 * steps/s (state.h TR_GAME_PACE_Q8), attract 14 (attract.h): 40 s of play
 * a zone; the attract loop is shorter so a passer-by sees every zone in
 * about 80 s. ponytail: calibration knobs, tune at the booth. */
#define TR_ZONE_STEPS_PLAY    800u
#define TR_ZONE_STEPS_ATTRACT 224u

/* Steps the gate is still reported past the runner line: the look blends
 * over the gate's pass (r3d_scene.c ZONE_BLEND_DZ) and must reach the new
 * zone before the gate is dropped. 12 steps = 132 model px = ~1,070 world
 * units behind the runner (test_zone.c checks it covers the blend). */
#define TR_ZONE_GATE_TAIL 12u

/* The row a gate spawns at, model px: nearer than the parts' TR_SPAWN_Y --
 * its approach from there (201 steps) plus the previous gate's tail (12) must
 * fit in an attract zone (224; the gate spawns only once the last one is
 * gone, that many steps before the zone ends) -- with 11 to spare. The
 * booth ramp speeds the steps, not the gate per step: at ramp r each of
 * those 213 steps counts 256/r of the zone, so the spare only grows (142
 * of 224 at 1.5x; test_ramp.c holds attract's 16 s under the ramp). The
 * scene fades it in there like a part, ~10 s out, on the visible far road.
 * On the TR_SCROLL_PX grid, so it lands on the runner line exactly. */
#define TR_ZONE_GATE_Y (-1100)
_Static_assert(TR_ZONE_GATE_Y % TR_SCROLL_PX == 0 && TR_ZONE_GATE_Y >= TR_SPAWN_Y,
               "the gate's spawn row");
_Static_assert((TR_TRACK_H_MAX - TR_RUNNER_H - TR_RUNNER_GROUND_MARGIN - TR_ZONE_GATE_Y +
                TR_SCROLL_PX - 1) /
                           TR_SCROLL_PX +
                       TR_ZONE_GATE_TAIL <
                   TR_ZONE_STEPS_ATTRACT,
               "a gate must spawn inside its zone, after the last one's tail");

/* tr_zone_t.gate_y / tr_frame_in_t.gate_y: no gate on the track. */
#define TR_ZONE_NO_GATE INT16_MIN

typedef struct {
	uint32_t steps; /* game steps since this zone was entered, at today's pace (ramp_q8) */
	uint32_t seq;   /* bumped on every zone entry, reset included (the HUD's popup cue) */
	int16_t
	    gate_y; /* model y of the gate to the next zone (like tr_entity_t.y), or TR_ZONE_NO_GATE */
	uint8_t zone; /* the zone the runner is in, 0 .. TR_ZONES - 1 */
	uint8_t frac; /* steps' fraction, Q8 */
	/* The world speed the next step runs at, Q8 (ramp.h tr_ramp_q8(); 0 or
	 * 256: today's pace). The caller sets it; a faster step counts for less
	 * of the zone, and the gate spawns its (faster) lead earlier, so a zone
	 * keeps its real time under the difficulty ramp. */
	uint16_t ramp_q8;
} tr_zone_t;

/* Display name, upper case (the HUD font). */
static inline const char *tr_zone_name(uint32_t zone)
{
	static const char *const names[TR_ZONES] = {
		"CIRCUIT BOARD", "CPU DIE CITY", "MEMORY CANYON", "ANTENNA FIELD", "NEON CITY"
	};

	return names[zone % TR_ZONES];
}

/* A new run: zone 0, no gate; an entry (seq bumps). */
static inline void tr_zone_reset(tr_zone_t *z)
{
	uint32_t seq = z->seq;

	*z        = (tr_zone_t){ 0 };
	z->seq    = seq + 1u;
	z->gate_y = TR_ZONE_NO_GATE;
}

/* Steps a gate takes from the spawn row to the runner line on a track of
 * height track_h (the same travel as an entity, step.c). */
static inline uint32_t tr_zone_gate_lead(int16_t track_h)
{
	return (uint32_t)((tr_runner_ground_y(track_h) - TR_ZONE_GATE_Y + TR_SCROLL_PX - 1) /
	                  TR_SCROLL_PX);
}

/* One game step (with tr_game_step(), at the same pace; attract selects the
 * shorter zones). The gate spawns at TR_ZONE_GATE_Y so it reaches the runner
 * line as the zone's steps run out; the step it reaches the line the runner
 * is in the next zone; TR_ZONE_GATE_TAIL steps later it is gone. Returns
 * true on a zone entry. */
static inline bool tr_zone_step(tr_zone_t *z, bool attract, int16_t track_h)
{
	uint32_t len   = attract ? TR_ZONE_STEPS_ATTRACT : TR_ZONE_STEPS_PLAY;
	int16_t  line  = tr_runner_ground_y(track_h);
	bool     entry = false;
	uint32_t r     = z->ramp_q8 != 0u ? z->ramp_q8 : 256u;
	uint32_t adv   = z->frac + 65536u / r; /* this step at today's pace, Q8 */

	z->steps += adv >> 8;
	z->frac = (uint8_t)adv;
	if (z->gate_y != TR_ZONE_NO_GATE) {
		int16_t was = z->gate_y;

		z->gate_y = (int16_t)(z->gate_y + TR_SCROLL_PX);
		if (was < line && z->gate_y >= line) {
			z->zone  = (uint8_t)((z->zone + 1u) % TR_ZONES);
			z->steps = 0;
			z->seq++;
			entry = true;
		} else if (z->gate_y >= line + (int16_t)(TR_ZONE_GATE_TAIL * TR_SCROLL_PX)) {
			z->gate_y = TR_ZONE_NO_GATE;
		}
	} else if (z->steps + tr_zone_gate_lead(track_h) * 256u / r >= len) {
		z->gate_y = TR_ZONE_GATE_Y;
	}
	return entry;
}

#endif /* TR_ZONE_H */
