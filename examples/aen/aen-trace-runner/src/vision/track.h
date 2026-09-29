/* src/vision/track.h */
#ifndef TR_TRACK_H
#define TR_TRACK_H

#include "../game/intent.h"

#define TR_TRACK_MIN_CONF   60 /* Below this the detection is not trusted at all. */
#define TR_TRACK_LOST_LIMIT 30 /* ~1 s at 30 Hz before declaring the player gone. */
#define TR_TRACK_HYST_PX    24 /* Dead band around a lane edge, in frame pixels. */

/*
 * Two physical-orientation assumptions about the camera, neither of which
 * is written down anywhere else in the repo, and neither of which is
 * verifiable without the board (whole-branch review F5).
 *
 * TR_CAM_MIRROR_X (0 = off): a camera FACING the player mirrors left/right
 * -- a player stepping to their own left appears at LARGER image x, so the
 * runner would move right unless this is set. Standing in front of a
 * screen, people expect mirror behaviour. 0 assumes the sensor or its
 * device-tree orientation already provides that mirror in hardware; set to
 * 1 if lateral control reads backwards on the bench.
 *
 * TR_CAM_FLIP_Y (0 = off): a 180-degree-mounted sensor (plausible if the
 * module ends up cabled to fit) reports every body vertically mirrored.
 * track.c mirrors the torso centre it measures (frame_h - cy) before
 * ANYTHING -- the rolling baseline, jump, duck -- reads it, so they stay
 * consistent with each other (a mirrored box's shoulders are its BOTTOM
 * edge, which track.c's shoulder-only fallback also accounts for). Set to
 * 1 if a real jump reads as a duck (and crouching as a jump) on the bench.
 *
 * Bench day: flip one of these, rebuild, done -- not "re-derive the
 * convention under pressure while people are waiting to see the demo".
 *
 * Guarded with #ifndef, not a plain #define: this is also how
 * tests/host/test_track_cam_orientation.c gets coverage of the `1` state of
 * both constants without a second copy of track.c -- runner.sh compiles
 * that one file (and the rest of the src/vision and src/game C sources with it)
 * with `-DTR_CAM_MIRROR_X=1 -DTR_CAM_FLIP_Y=1` on the command line. Every
 * other test file, and the real firmware build, gets the 0/0 default below
 * (whole-branch fix round B, B2 -- "neither has any test coverage in
 * either state").
 */
#ifndef TR_CAM_MIRROR_X
#define TR_CAM_MIRROR_X 0
#endif
#ifndef TR_CAM_FLIP_Y
#define TR_CAM_FLIP_Y 0
#endif

/*
 * Intent from the TORSO, scale-invariant (the silicon finding of
 * 2026-09-25: with the legs out of frame, the old box-top-vs-stance rule
 * read a player walking TOWARD the camera as a 4 s jump, against a baseline
 * captured once at boot).
 *
 * The box track.c reads (pose.c's tr_pose_box()):
 *   x + w/2 : torso centre x          -> the lane;
 *   y       : shoulder-mid y;
 *   h       : torso length, shoulder-mid to hip-mid (> 0), the SCALE s, and
 *             cy = y + h/2 the torso centre; or 0 when the hips are not seen,
 *             then the scale comes from the shoulder width w against the
 *             baseline's (s = base_s * w / base_w; on first sight s = w),
 *             cy = y + s/2. A width under TR_TRACK_MIN_SCALE (a side-on
 *             player: both shoulders on one point) is a lane-only frame.
 *             detect.c's whole-body box is fed this way too (h = 0, see
 *             main.c): its width is the scale, its top the position, so a
 *             crouch (top down, width kept) reads as a duck.
 * All thresholds are fractions of s, so they hold at any distance.
 *
 * JUMP = translation, not growth, and only with the hips seen: against the
 *   window's lowest frame (the last TR_TRACK_WIN poses, ~300 ms) the
 *   shoulder-mid y AND the hip-mid y each rise by more than
 *   TR_TRACK_JUMP_K_PCT of s, the torso length changes by less than
 *   TR_TRACK_JUMP_DH_PCT of that rise (|dh| < 0.25 dcy; dh against the low
 *   frame AND against the baseline length, the larger), and the shoulder
 *   width stays within TR_TRACK_SCALE_TOL_PCT of the baseline's; cy must
 *   also be above the baseline by the same K. Walking closer lifts the torso
 *   too (on 2026W36-0009 dcy ~ 3.2 ds), but grows it and widens the shoulders with
 *   it. No jump for TR_TRACK_WIN poses after a mid-stream re-seed (still
 *   moving; the new baseline width is one pose). Held until cy is back
 *   within half the threshold, at most TR_TRACK_JUMP_MAX.
 * The width both gestures test is the median of the last three poses': on
 *   2026W36-0009 one pose's shoulder width spreads 41..66 px standing still.
 * DUCK = cy drops below the baseline by more than TR_TRACK_DUCK_K_PCT of s,
 *   s not grown by more than TR_TRACK_SCALE_TOL_PCT, and the shoulder width
 *   within TR_TRACK_SCALE_TOL_PCT of the baseline's: a squat or a lean keeps
 *   the width, a step back (which also sinks and shrinks the torso) narrows
 *   it. Held until back within half, or at most TR_TRACK_DUCK_MAX: then the
 *   pose becomes the new baseline (whatever it was, it is not a duck any
 *   more).
 * Both: the condition on TR_TRACK_DEBOUNCE consecutive poses to start,
 *   then at least TR_TRACK_HOLD_MIN poses on; no duck for
 *   TR_TRACK_LAND_COOLDOWN_FRAMES after a jump ends (the landing crouch).
 * Rolling baseline: an EMA (1/TR_TRACK_BASE_DIV per pose, Q4) of cy, s and
 *   the shoulder width, on every pose with no gesture active or pending (with
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
 * re-reports the lane and the gesture state; the debounce, the window, the
 * holds and the EMA advance on a new pose only -- one outlier re-read on two
 * ticks is still one outlier. At most one new pose per tick, so the ~ms
 * figures below are at 30 poses/s. ponytail: tuning knobs, measured on the
 * 2026W36-0009 captures; retune here on glass.
 */
#define TR_TRACK_JUMP_K_PCT           25 /* jump: torso centre up by this % of s */
#define TR_TRACK_DUCK_K_PCT           30 /* duck: torso centre down by this % of s */
#define TR_TRACK_SCALE_TOL_PCT        10 /* "scale about constant", against the baseline */
#define TR_TRACK_REBASE_PCT           30 /* s this far off the baseline: re-baseline */
#define TR_TRACK_JUMP_DH_PCT          25 /* jump: |torso length change| under this % of the rise */
#define TR_TRACK_WIN                  9  /* jump rise window, frames (~300 ms) */
#define TR_TRACK_DEBOUNCE             2  /* consecutive frames to start a gesture */
#define TR_TRACK_HOLD_MIN             5  /* a lamp stays on at least ~170 ms */
#define TR_TRACK_JUMP_MAX             24 /* a jump ends after ~0.8 s, even if held */
#define TR_TRACK_DUCK_MAX             45 /* a duck ends after ~1.5 s: re-baseline */
#define TR_TRACK_LAND_COOLDOWN_FRAMES 6  /* ~200 ms landing crouch: not a duck */
#define TR_TRACK_BASE_DIV             16 /* baseline EMA: ~1 s to settle */
#define TR_TRACK_REACQ_FRAMES         8  /* bodiless frames that make the next one a new person */
#define TR_TRACK_MIN_SCALE            8  /* px: a smaller scale measures nothing */

typedef struct {
	int16_t  x, y, w, h;
	uint8_t  confidence;
	bool     valid;
	uint32_t seq; /* the pslot seq it was published under; 0 = none, every box is new */
} tr_box_t;

typedef struct {
	int16_t lane_edges[2];
	int16_t frame_w; /* Stored for TR_CAM_MIRROR_X; set alongside lane_edges. */
	int16_t frame_h; /* Stored for TR_CAM_FLIP_Y; set once by tr_track_init(). */
	bool    calibrated;
	uint8_t lane;
	uint8_t lost_frames;
	/* Rolling baseline, Q4 px; valid when `based`. */
	bool     based;
	int32_t  base_cy, base_s, base_w;
	uint32_t last_seq; /* tr_box_t.seq of the last pose measured */
	/* The last TR_TRACK_WIN measured poses: torso centre, shoulder-mid and
	 * hip-mid y (after TR_CAM_FLIP_Y), the torso length (0: no hips) and
	 * the shoulder width. */
	int16_t win_cy[TR_TRACK_WIN], win_sho[TR_TRACK_WIN], win_hip[TR_TRACK_WIN], win_h[TR_TRACK_WIN],
	    win_w[TR_TRACK_WIN];
	uint8_t win_n, win_i;
	/* Gestures: pending = consecutive frames the condition held; active =
	 * frames since it started (0 = off). */
	uint8_t jump_pend, duck_pend, jump_on, duck_on, land_cooldown;
	uint8_t settle;      /* poses left with no jump after a mid-stream re-seed */
	int16_t jump_ref_cy; /* torso centre at the jump's take-off */
} tr_track_t;

void        tr_track_init(tr_track_t *t, int16_t frame_w, int16_t frame_h);
void        tr_track_calibrate(tr_track_t *t, tr_box_t b, int16_t frame_w);
tr_intent_t tr_track_update(tr_track_t *t, tr_box_t b);
bool        tr_track_player_lost(const tr_track_t *t);

/*
 * Force the tracker's lane belief back into agreement with the game's actual
 * current lane (0..2).  tr_track_update() only ever reports a *delta*, so if
 * a caller ever computes an intent it does not apply -- the game is paused,
 * over, or otherwise drops a tick -- the tracker has no way to know its delta
 * was not honoured, and its internal `lane` permanently desyncs from the
 * game's.  The caller must call this on every such discontinuity: at minimum
 * on a run reset and on leaving pause, passing the lane the game is actually
 * showing.  This only corrects the tracker's own bookkeeping; it is not a
 * substitute for the game's own edge clamping, and does not touch anything
 * else (calibration, stance, cooldown).
 */
void tr_track_resync(tr_track_t *t, uint8_t lane);

#endif /* TR_TRACK_H */
