/* tests/host/test_zone.c -- the zone schedule (P15, src/game/zone.h): a pure
 * function of the game steps since tr_zone_reset(), the zones in order, one
 * entry every TR_ZONE_STEPS_* steps (attract shorter), the gate reaching the
 * runner line exactly on the entry step and leaving TR_ZONE_GATE_TAIL steps
 * later. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../src/game/zone.h"

#define TRACK_H 1280

int main(void)
{
	const int16_t line = tr_runner_ground_y(TRACK_H);

	/* 1. The sequence: every zone in order, then back to the board, one
	 * entry every zone length; the gate is on the line on the entry step,
	 * spawned lead steps before, dropped TR_ZONE_GATE_TAIL steps after. */
	for (int attract = 0; attract < 2; attract++) {
		uint32_t  len = attract ? TR_ZONE_STEPS_ATTRACT : TR_ZONE_STEPS_PLAY, last = 0, entries = 0;
		tr_zone_t z      = { 0 };
		int16_t   prev_y = TR_ZONE_NO_GATE;

		tr_zone_reset(&z);
		assert(z.zone == 0u && z.gate_y == TR_ZONE_NO_GATE && z.seq == 1u);
		for (uint32_t s = 1; s <= len * (TR_ZONES + 1); s++) {
			uint8_t was   = z.zone;
			bool    entry = tr_zone_step(&z, attract, TRACK_H);

			if (z.gate_y != TR_ZONE_NO_GATE && prev_y != TR_ZONE_NO_GATE) {
				assert(z.gate_y == prev_y + TR_SCROLL_PX); /* moves with the track */
			}
			if (prev_y == TR_ZONE_NO_GATE && z.gate_y != TR_ZONE_NO_GATE) {
				assert(z.gate_y == TR_ZONE_GATE_Y && s - last + tr_zone_gate_lead(TRACK_H) == len);
			}
			if (prev_y != TR_ZONE_NO_GATE && z.gate_y == TR_ZONE_NO_GATE) {
				assert(s - last == TR_ZONE_GATE_TAIL); /* dropped after the tail */
			}
			if (entry) {
				assert(z.zone == (was + 1u) % TR_ZONES && s - last == len && z.gate_y >= line &&
				       z.gate_y < line + TR_SCROLL_PX);
				assert(z.seq == 1u + ++entries);
				last = s;
			} else {
				assert(z.zone == was);
				assert(z.gate_y == TR_ZONE_NO_GATE || z.gate_y < line ||
				       z.gate_y < line + (int16_t)(TR_ZONE_GATE_TAIL * TR_SCROLL_PX));
			}
			prev_y = z.gate_y;
		}
		assert(entries == TR_ZONES + 1 && z.zone == 1u);
		printf("zones (%s): an entry every %u steps, gate lead %u steps, %u entries\n",
		       attract ? "attract" : "play",
		       (unsigned)len,
		       (unsigned)tr_zone_gate_lead(TRACK_H),
		       (unsigned)entries);
	}

	/* 2. Deterministic: the same steps give the same states; a reset
	 * restarts the schedule (a new run meets its gates at the same places)
	 * and counts as an entry. */
	{
		tr_zone_t a = { 0 }, b = { 0 };

		tr_zone_reset(&a);
		tr_zone_reset(&b);
		for (int s = 0; s < 3000; s++) {
			bool at = (s / 700) & 1; /* attract and play stretches */

			assert(tr_zone_step(&a, at, TRACK_H) == tr_zone_step(&b, at, TRACK_H));
			assert(memcmp(&a, &b, sizeof(a)) == 0);
		}
		uint32_t seq = a.seq;

		tr_zone_reset(&a);
		assert(a.zone == 0u && a.steps == 0u && a.gate_y == TR_ZONE_NO_GATE && a.seq == seq + 1u);
	}

	/* 3. Names: one per zone, distinct, upper case (the HUD font). */
	for (uint32_t i = 0; i < TR_ZONES; i++) {
		const char *n = tr_zone_name(i);

		assert(n[0] != '\0');
		for (const char *p = n; *p; p++) {
			assert((*p >= 'A' && *p <= 'Z') || *p == ' ' || *p == '/');
		}
		for (uint32_t j = 0; j < i; j++) {
			assert(strcmp(n, tr_zone_name(j)) != 0);
		}
	}
	printf("zones: %d, named\n", TR_ZONES);
	return 0;
}
