#ifndef TR_INTENT_H
#define TR_INTENT_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The ONLY vocabulary between an input device and the game.  Tilt and body
 * tracking both reduce to this, so the game never learns which one is driving
 * and a new input never reaches into game state.
 */
typedef enum {
	TR_INPUT_NONE = 0,
	TR_INPUT_TILT,
	TR_INPUT_VISION,
	TR_INPUT_ATTRACT, /**< Synthetic: the game driving itself -- see game/attract.h. */
} tr_input_source_t;

typedef struct {
	int8_t            lane_delta; /**< -1 left, 0 hold, +1 right. */
	bool              jump;
	bool              duck;
	tr_input_source_t source;
} tr_intent_t;

static inline tr_intent_t tr_intent_none(void)
{
	return (tr_intent_t){ .lane_delta = 0, .jump = false, .duck = false, .source = TR_INPUT_NONE };
}

/*
 * Intents gathered over the frames between two paced game steps (state.h
 * TR_GAME_PACE_Q8): a jump or duck asked on any of them is kept, so none is
 * lost on a frame that does not step. Lane moves do not wait: tr_play_frame()
 * applies each on its own frame, and a lane here is just added up.
 */
static inline tr_intent_t tr_intent_merge(tr_intent_t held, tr_intent_t now)
{
	int lane = held.lane_delta + now.lane_delta;

	held.jump       = held.jump || now.jump;
	held.duck       = held.duck || now.duck;
	held.lane_delta = (int8_t)(lane > 2 ? 2 : lane < -2 ? -2 : lane);
	if (now.source != TR_INPUT_NONE) {
		held.source = now.source;
	}
	return held;
}

#endif /* TR_INTENT_H */
