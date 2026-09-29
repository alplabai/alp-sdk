/* src/game/ramp.h -- the in-run difficulty ramp (booth): the world speeds up
 * gently with the distance a run has covered, pure and header-only
 * (tests/host/test_ramp.c).
 *
 * The ramp scales the PACE -- game steps per presented frame -- not the step
 * itself: a step still scrolls TR_SCROLL_PX, spawns every TR_SPAWN_TICKS
 * and times a jump in steps, so the game (and the attract AI, which acts
 * per step) is step for step the same at any speed, collisions included,
 * and the spawns come faster in real time with the world ("spawn density
 * follows"). The A32 interpolates any pace from the packet's phase and
 * pace_q8. What runs in frames stays in real time: the crash sequence,
 * the tilt thresholds, the reactions; the zones are held to their real
 * time through tr_zone_t.ramp_q8 (zone.h).
 *
 * Speed factor, Q8 (256 = today's pace): linear in the run's steps from
 * 1.0 at the start to TR_RAMP_MAX_Q8 at TR_RAMP_STEPS, then flat.
 * 1.5x is the ceiling: at a 30 Hz panel today's 0.5x pace is 0.667 steps a
 * frame, and the main loop takes at most one step a frame (state.h). At
 * today's 20 steps/s the cap arrives after ~90 s of play:
 * TR_RAMP_STEPS / 20 * ln(1.5) / 0.5 s. ponytail: calibration knobs, tune
 * at the booth.
 */
#ifndef TR_RAMP_H
#define TR_RAMP_H

#include <stdint.h>

#include <stdbool.h>

#include "attract.h" /* TR_ATTRACT_SPEED_Q16 */
#include "state.h"

#ifndef TR_RAMP_MAX_Q8
#define TR_RAMP_MAX_Q8 384u /* 1.5x */
#endif
#ifndef TR_RAMP_STEPS
#define TR_RAMP_STEPS 2220u /* ~90 s of play to the cap */
#endif
_Static_assert(TR_RAMP_MAX_Q8 >= 256u && TR_RAMP_MAX_Q8 <= 384u, "1.0x .. 1.5x: never two steps a frame at 30 Hz");

/* The speed factor after `steps` steps of the run, Q8. */
static inline uint32_t tr_ramp_q8(uint32_t steps)
{
	if (steps >= TR_RAMP_STEPS) {
		return TR_RAMP_MAX_Q8;
	}
	return 256u + (TR_RAMP_MAX_Q8 - 256u) * steps / TR_RAMP_STEPS;
}

/* A base pace (Q0.16 steps a frame: TR_PLAY_SPEED_Q16, TR_ATTRACT_SPEED_Q16)
 * ramped for `steps` steps into the run; never above one step a frame. */
static inline uint32_t tr_ramp_speed_q16(uint32_t base_q16, uint32_t steps)
{
	uint32_t s = (base_q16 * tr_ramp_q8(steps) + 128u) >> 8;

	return s > 65536u ? 65536u : s;
}

/* The world speed a presented frame runs at: play or attract's base pace,
 * ramped by the run's steps so far (main.c). */
static inline uint32_t tr_ramp_frame_q16(bool attract, uint32_t steps)
{
	return tr_ramp_speed_q16(attract ? TR_ATTRACT_SPEED_Q16 : TR_PLAY_SPEED_Q16, steps);
}

/* That speed as the packet's pace_q8: 255 at most -- 256 (one step a
 * frame) would wrap to 0, which the renderer reads as an old HE's "no
 * pace" (tr_mbox.h). */
static inline uint8_t tr_ramp_pace_q8(uint32_t speed_q16)
{
	uint32_t q8 = speed_q16 >> 8;

	return (uint8_t)(q8 > 255u ? 255u : q8);
}

/* The speed the step that just ran (g->tick after it) was taken at: what
 * tr_zone_t.ramp_q8 wants before tr_zone_step(). */
static inline uint16_t tr_ramp_stepped_q8(uint32_t tick_after)
{
	return (uint16_t)tr_ramp_q8(tick_after != 0u ? tick_after - 1u : 0u);
}

#endif /* TR_RAMP_H */
