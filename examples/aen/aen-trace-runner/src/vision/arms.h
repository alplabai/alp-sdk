/* src/vision/arms.h -- the arm-raise controls: raise the player's LEFT arm to
 * move one lane left, the RIGHT arm for one lane right, BOTH arms together to
 * jump. Pure C, no floating point; host-tested (tests/host/test_arms.c).
 *
 * Why arms, not a step: standing and stepping sideways needs floor space and
 * reads a lane from where the body is, which drifts with the player's
 * distance and with who is in front of the camera. A raised arm is a clear,
 * distance-independent, repeatable gesture, and it keeps the player's feet
 * where they were calibrated.
 *
 * The pipeline, one stage per file:
 *   pose.c  tr_pose_box() measures how high each wrist is above its own
 *           shoulder (tr_box_t.arm_raise, below) and decides which arm is the
 *           PLAYER's left (the mirror question, see pose.h);
 *   arms.c  tr_arms_step() turns two raise levels per new pose into
 *           one-shot events (this file);
 *   track.c feeds them into tr_intent_t.lane_delta / .jump.
 */
#ifndef TR_ARMS_H
#define TR_ARMS_H

#include <stdbool.h>
#include <stdint.h>

/* "Raised" = the wrist is above the SAME-side shoulder by more than this
 * fraction of the shoulder width (%). A relative margin keeps the gesture the
 * same at any distance from the camera. An arm held out level with the
 * shoulder reads ~0, one pointing straight up ~150-200. */
#define TR_ARM_UP_PCT 50

/* An arm counts as lowered, and so re-arms, only below this (%). The gap to
 * TR_ARM_UP_PCT is the hysteresis: a wrist hovering near the raise line fires
 * once, not on every pose that wobbles across it. */
#define TR_ARM_DOWN_PCT 20

/* A shoulder pair narrower than this (px) is a side-on player (or one
 * keypoint stacked on the other): the margin above is relative to it, so it
 * measures nothing. */
#define TR_ARM_MIN_SHOULDER_PX 16

/* Keypoint confidence gate for the arm controls: the shoulder AND the wrist of
 * an arm must each score at least this (0..255), or that arm is "unknown" and
 * does nothing. The skeleton overlay's gate (pose.h TR_POSE_KP_MIN) -- a bone
 * the screen would not draw is not one the game acts on. */
#define TR_ARM_KP_MIN 77

/* Simultaneity window, in NEW poses (~33 ms each at the HP's 30 Hz). A lane
 * step waits until its arm has been up this many poses with the other arm
 * still down; the other arm joining inside that window makes the gesture a
 * JUMP instead and cancels the lane step. Both arms going up "together" by
 * hand are rarely closer than a pose or two apart, so this is what keeps a
 * jump from first stepping a lane. Cost: a lane step lands
 * (TR_ARM_SETTLE_POSES - 1) poses (~100 ms) after the wrist crosses the line.
 * The window is TIME: the age counts every new pose since the rise, whether or
 * not the wrist is judged or in the hysteresis band meanwhile. When it runs
 * out, a wrist that is still clearly up steps its lane; one that left the
 * frame (UNKNOWN) or fell back into the band never confirmed the gesture, so
 * the rise is dropped and a later rise starts afresh.
 *
 * A second arm that rises while the first is SPENT and still up (a staggered
 * both-arms raise, later than the window) is a jump too: the first arm's lane
 * step has already happened, the second turns it into the jump the player
 * meant.
 * ponytail: tuning knob, retune on glass. */
#define TR_ARM_SETTLE_POSES 4

/* tr_box_t.arm_raise[k] when that arm cannot be judged this pose. */
#define TR_ARM_UNKNOWN INT16_MIN

enum {
	TR_ARM_LEFT = 0, /* the PLAYER's left arm (as the player sees themselves) */
	TR_ARM_RIGHT,
};

typedef struct {
	bool    primed[2]; /* this arm has been judged since the last reset */
	uint8_t state[2];  /* per arm: DOWN, RISING (counting), SPENT (up, already used) */
	uint8_t age[2];    /* new poses since a RISING arm crossed the line (counted on every pose) */
} tr_arms_t;

typedef struct {
	int8_t lane_delta; /* -1 left, 0 none, +1 right: ONE tick only */
	bool   jump;       /* ONE tick only */
} tr_arm_event_t;

/* Both arms have been judged since the last reset (tests, bench prints). */
static inline bool tr_arms_primed(const tr_arms_t *a)
{
	return a->primed[0] && a->primed[1];
}

/* Forget everything: the first pose that JUDGES an arm (not TR_ARM_UNKNOWN)
 * only records where it is, per arm. An arm that is already up then is spent
 * -- it does not fire until it has been lowered and raised again. Call it wherever the player may have changed
 * (a run starts, pause ends, the player is re-acquired). */
void tr_arms_reset(tr_arms_t *a);

/* One NEW pose's raise levels (tr_box_t.arm_raise: [TR_ARM_LEFT],
 * [TR_ARM_RIGHT], % of shoulder width, TR_ARM_UNKNOWN when unjudged). Edge
 * triggered: an arm fires once when it goes up and not again until it has
 * dropped below TR_ARM_DOWN_PCT. */
tr_arm_event_t tr_arms_step(tr_arms_t *a, const int16_t raise[2]);

#endif /* TR_ARMS_H */
