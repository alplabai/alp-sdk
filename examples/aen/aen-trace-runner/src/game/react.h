/* src/game/react.h -- the runner's reactions (P16), game side.
 *
 * The HE turns the game's events into ONE current reaction the frame packet
 * carries (tr_mbox.h TR_REACT_*, react_side / react_seq / react_ms); the
 * A32 animates it (r3d_scene.c: glance back, fist pump, stumble, spin) from
 * the packet alone. Pure and deterministic, host-tested (test_react.c):
 *   - which event: combo milestone (x3 / x5 reached) > near miss >
 *     pickup > pass; a stronger (or equal) event replaces the current
 *     reaction, a weaker one only once it is TR_REACT_HOLD_US old, and a
 *     pass never within TR_REACT_PASS_GAP_US of the last reaction (a glance
 *     every obstacle would be a tic);
 *   - the side: where the obstacle went by, -1 left / +1 right of the
 *     runner's lane; in its own lane (jumped / ducked) alternating;
 *   - the clock: real time, TR_PANEL_PERIOD_US a presented frame.
 */
#ifndef TR_REACT_H
#define TR_REACT_H

#include <stdint.h>

#include "state.h"

#define TR_REACT_HOLD_US     900000u
#define TR_REACT_PASS_GAP_US 1500000u
#define TR_REACT_MAX_US      70000000u /* the clock saturates (react_ms 65535 in the packet) */

typedef struct {
	uint8_t  kind;  /* TR_REACT_* (tr_mbox.h) */
	int8_t   side;  /* -1 left, +1 right */
	uint8_t  seq;   /* +1 per reaction taken */
	uint8_t  combo; /* the combo last seen: a milestone is the step it reaches x3 / x5 */
	uint32_t us;    /* since the reaction started */
} tr_react_t;

/* A new run: no reaction (the clock parked at its maximum). */
void tr_react_init(tr_react_t *r);

/* After each tr_game_step() + tr_score_step() of a run: g->ev / ev_lane /
 * lane of that step and the score's combo after it. */
void tr_react_step(tr_react_t *r, const tr_game_t *g, uint8_t combo);

/* Once per presented frame. */
void tr_react_frame(tr_react_t *r);

/* The clock as the packet carries it: ms, saturating at 65535 (inline: the
 * A32 renderer links tr_mbox.c, not this module). */
static inline uint16_t tr_react_ms(const tr_react_t *r)
{
	uint32_t ms = r->us / 1000u;

	return (uint16_t)(ms > 65535u ? 65535u : ms);
}

#endif /* TR_REACT_H */
