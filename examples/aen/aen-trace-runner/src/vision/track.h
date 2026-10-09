/* src/vision/track.h */
#ifndef TR_TRACK_H
#define TR_TRACK_H

#include "../game/intent.h"
#include "arms.h"

#define TR_TRACK_MIN_CONF   60 /* Below this the detection is not trusted at all. */
#define TR_TRACK_LOST_LIMIT 30 /* ~1 s at 30 Hz before declaring the player gone. */

/*
 * TR_CAM_FLIP_Y (0 = off): a 180-degree-mounted sensor (plausible if the
 * module ends up cabled to fit) reports every body vertically mirrored.
 * track.c mirrors the torso centre it measures (frame_h - cy) before
 * ANYTHING -- the rolling baseline, the duck -- reads it, so they stay
 * consistent with each other (a mirrored box's shoulders are its BOTTOM
 * edge, which track.c's shoulder-only fallback also accounts for), and
 * pose.c reads a wrist "above" its shoulder the other way round. Set to 1 if
 * a real crouch reads as a stand (and standing as a duck) on the bench.
 *
 * Guarded with #ifndef, not a plain #define: this is also how
 * tests/host/test_track_cam_orientation.c gets coverage of the `1` state
 * without a second copy of track.c -- runner.sh compiles that one file (and
 * the rest of the src/vision and src/game C sources with it) with
 * `-DTR_CAM_FLIP_Y=1` on the command line. Every other test file, and the
 * real firmware build, gets the 0 default below.
 */
#ifndef TR_CAM_FLIP_Y
#define TR_CAM_FLIP_Y 0
#endif

/*
 * Intent from the body. Two independent reads of the same pose:
 *
 * ARMS (arms.h) -- lane and jump: the player's left arm up is one lane left,
 *   the right arm one lane right, both arms together a jump. tr_box_t carries
 *   each arm's raise level (pose.c measures it, and decides which arm is the
 *   player's left under TR_CAM_MIRROR); tr_arms_step() makes the events.
 *   Nothing here reads where the player STANDS, so there is no lane to
 *   calibrate and no floor to walk on.
 *
 * TORSO -- the duck, scale-invariant (the silicon finding of 2026-09-25: with
 *   the legs out of frame, the old box-top-vs-stance rule read a player
 *   walking TOWARD the camera as a 4 s jump, against a baseline captured once
 *   at boot).
 *
 * The box track.c reads (pose.c's tr_pose_box()):
 *   x + w/2 : torso centre x;
 *   y       : shoulder-mid y;
 *   h       : torso length, shoulder-mid to hip-mid (> 0), the SCALE s, and
 *             cy = y + h/2 the torso centre; or 0 when the hips are not seen,
 *             then the scale comes from the shoulder width w against the
 *             baseline's (s = base_s * w / base_w; on first sight s = w),
 *             cy = y + s/2. A width under TR_TRACK_MIN_SCALE (a side-on
 *             player: both shoulders on one point) measures no duck.
 *             detect.c's whole-body box is fed this way too (h = 0, see
 *             main.c): its width is the scale, its top the position, so a
 *             crouch (top down, width kept) reads as a duck. It has no
 *             keypoints, so it carries no arm levels: only the pose path
 *             (TR_INPUT_NPU) has the lane and jump controls.
 * All thresholds are fractions of s, so they hold at any distance.
 *
 * DUCK = cy drops below the baseline by more than TR_TRACK_DUCK_K_PCT of s,
 *   s not grown by more than TR_TRACK_SCALE_TOL_PCT, and the shoulder width
 *   (median of the last three poses: on 2026W36-0009 one pose's width spreads
 *   41..66 px standing still) within TR_TRACK_SCALE_TOL_PCT of the
 *   baseline's: a squat or a lean keeps the width, a step back (which also
 *   sinks and shrinks the torso) narrows it. The condition on
 *   TR_TRACK_DEBOUNCE consecutive poses to start, then at least
 *   TR_TRACK_HOLD_MIN poses on. Held until back within half, or at most
 *   TR_TRACK_DUCK_MAX: then the pose becomes the new baseline (whatever it
 *   was, it is not a duck any more).
 * Rolling baseline: an EMA (1/TR_TRACK_BASE_DIV per pose, Q4) of cy, s and
 *   the shoulder width, on every pose with no duck active or pending (with
 *   the hips unseen s is base_s * w / base_w, so both move by the same
 *   factor and the ratio holds). Re-seeded from the current pose when the
 *   player is re-acquired after TR_TRACK_REACQ_FRAMES without a body, or
 *   when s leaves +-TR_TRACK_REBASE_PCT of the baseline (someone else, or
 *   much closer; during a held duck, shrinking to half is still the crouch).
 *   tr_track_calibrate() takes no baseline: the first tracked pose seeds it.
 *
 * Counts are POSES, not HE ticks: main.c hands the tracker the last
 * accepted pose on every ~30 Hz tick, re-read until the HP publishes a new
 * one, and tags it with its pslot seq (tr_box_t.seq). A repeated seq only
 * re-reports the duck state; the debounce, the holds, the EMA and the arm
 * edges advance on a new pose only -- one outlier re-read on two ticks is
 * still one outlier, and an arm raise is never counted twice. At most one
 * new pose per tick, so the ~ms figures below are at 30 poses/s.
 * ponytail: tuning knobs, measured on the 2026W36-0009 captures; retune here
 * on glass.
 */
#define TR_TRACK_DUCK_K_PCT    30 /* duck: torso centre down by this % of s */
#define TR_TRACK_SCALE_TOL_PCT 10 /* "scale about constant", against the baseline */
#define TR_TRACK_REBASE_PCT    30 /* s this far off the baseline: re-baseline */
#define TR_TRACK_DEBOUNCE      2  /* consecutive frames to start a duck */
#define TR_TRACK_HOLD_MIN      5  /* a lamp stays on at least ~170 ms */
#define TR_TRACK_DUCK_MAX      45 /* a duck ends after ~1.5 s: re-baseline */
#define TR_TRACK_BASE_DIV      16 /* baseline EMA: ~1 s to settle */
#define TR_TRACK_REACQ_FRAMES  8  /* bodiless frames that make the next one a new person */
#define TR_TRACK_MIN_SCALE     8  /* px: a smaller scale measures nothing */

typedef struct {
	int16_t  x, y, w, h;
	uint8_t  confidence;
	bool     valid;
	uint32_t seq; /* the pslot seq it was published under; 0 = none, every box is new */
	/* How high each wrist is above its own shoulder, % of the shoulder width,
	 * [TR_ARM_LEFT] = the PLAYER's left (arms.h); TR_ARM_UNKNOWN when it
	 * cannot be judged. Zero (level with the shoulder = down) on a box that
	 * has no keypoints, so only tr_pose_box() ever raises an arm. */
	int16_t arm_raise[2];
} tr_box_t;

typedef struct {
	int16_t frame_h; /* Stored for TR_CAM_FLIP_Y; set once by tr_track_init(). */
	bool    calibrated;
	uint8_t lost_frames;
	/* Rolling baseline, Q4 px; valid when `based`. */
	bool     based;
	int32_t  base_cy, base_s, base_w;
	uint32_t last_seq; /* tr_box_t.seq of the last pose measured */
	/* The shoulder widths of the last three measured poses. */
	int16_t win_w[3];
	uint8_t win_n, win_i;
	/* Duck: pending = consecutive frames the condition held; active = frames
	 * since it started (0 = off). */
	uint8_t   duck_pend, duck_on;
	tr_arms_t arms;
} tr_track_t;

void        tr_track_init(tr_track_t *t, int16_t frame_h);
void        tr_track_calibrate(tr_track_t *t, tr_box_t b);
tr_intent_t tr_track_update(tr_track_t *t, tr_box_t b);
bool        tr_track_player_lost(const tr_track_t *t);

/*
 * Forget the arm edges: the next pose only records where the arms are, and an
 * arm already up does not fire until it is lowered and raised again.
 * tr_track_update() produces an intent the caller may not apply -- the game
 * is paused, over, or otherwise drops a tick -- and a raised arm must not
 * turn into a lane step or a jump the moment play resumes. The caller must
 * call this on every such discontinuity: a run reset, leaving pause, leaving
 * attract. Nothing else is touched (calibration, baseline).
 */
void tr_track_resync(tr_track_t *t);

#endif /* TR_TRACK_H */
