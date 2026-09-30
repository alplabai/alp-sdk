/* src/vision/pose.h */
#ifndef TR_POSE_H
#define TR_POSE_H

#include <stdbool.h>
#include <stdint.h>

#include "track.h"

/*
 * NPU body control: the M55-HP runs MoveNet SinglePose Lightning on the
 * camera and publishes one tr_pose_t per frame; this file turns that pose
 * into a tr_box_t (the torso box, track.h's contract), so track.c's lanes,
 * hysteresis and jump/duck -- and attract/mode on top of them -- are shared
 * with the classical detector (detect.c).
 * Design: docs/superpowers/specs/2026-09-24-npu-body-control-design.md.
 *
 * Pure C, no floating point, host-tested (tests/host/test_pose.c,
 * test_pose_clip.c).
 */

#define TR_POSE_KP 17 /* MoveNet / COCO keypoint order, below. */

enum {
	TR_KP_NOSE = 0,
	TR_KP_LEYE,
	TR_KP_REYE,
	TR_KP_LEAR,
	TR_KP_REAR,
	TR_KP_LSHO,
	TR_KP_RSHO,
	TR_KP_LELB,
	TR_KP_RELB,
	TR_KP_LWRI,
	TR_KP_RWRI,
	TR_KP_LHIP,
	TR_KP_RHIP,
	TR_KP_LKNE,
	TR_KP_RKNE,
	TR_KP_LANK,
	TR_KP_RANK,
};

/* A keypoint counts at or above this score (0..255; 77 = 0.30). On the
 * host clip a present player's torso keypoints score 0.26..0.60, an empty
 * booth's 0.03..0.07 (docs/superpowers/specs/2026-09-24-npu-body-control-design.md).
 * ponytail: calibration knob, tune on glass. */
#define TR_POSE_KP_MIN 77

/* Stillness that arms calibration: the box must hold within
 * TR_STILL_TOL_PCT of its own height (the torso length) for TR_STILL_FRAMES
 * frames (1 s at the 30 Hz pose rate). */
#define TR_STILL_FRAMES  30
#define TR_STILL_TOL_PCT 15

typedef struct {
	int16_t x,
	    y; /* UPRIGHT camera frame px (cam_rot.h: 400x640 with the sensor on its side, 640x400 at 0) */
	uint8_t score; /* 0..255 */
} tr_kp_t;

typedef struct {
	tr_kp_t kp[TR_POSE_KP];
} tr_pose_t;

/*
 * Pose -> torso box for track.c (its contract: track.h):
 *   x + w/2 : torso centre x (shoulder-mid and hip-mid averaged), so a raised
 *             or outstretched arm never moves the lane;
 *   w       : shoulder width (0 unless both shoulders are confident);
 *   y       : shoulder-mid y;
 *   h       : hip-mid y - shoulder-mid y, the torso length; 0 when no hip is
 *             confident (a player close enough that the frame cuts them).
 *   confidence: mean score of the keypoints used.
 * Head, wrists, knees and ankles are never read: a head turn, raised arms or
 * legs out of frame change nothing. Invalid without a confident shoulder.
 */
tr_box_t tr_pose_box(const tr_pose_t *p);

/*
 * "A player is here" -- the one presence rule the booth flow (main.c:
 * attract, the join lobby, a run's end, the initials) decides on. A single
 * box is not enough: on 2026W36-0009 an EMPTY room's MoveNet output flickers a lone
 * shoulder or hip up to TR_POSE_KP_MIN (13 of 31 samples over 60 s), and one
 * such box every few seconds used to count as a player -- the game never
 * reached attract and played phantom runs forever. Present = a TORSO (a
 * confident shoulder AND a confident hip below it: tr_pose_box()'s h > 0) on
 * at least TR_PRESENT_MIN of the last TR_PRESENT_WIN poses. Counted per NEW
 * pose (box seq; seq 0 = every box is new), so the HE re-reading one pose
 * every tick counts it once; an invalid box (no pose, stale slot) counts as
 * a pose without a torso. The recorded standing player (tests/host/
 * tr_pose_silicon.h) shows a torso on 25 of 26 poses; the empty room on 1
 * of 31. `need_torso` false counts any confident box instead: the classical
 * whole-body detector (no hips, h is always 0), and mid-run on the pose path
 * -- the torso is the bar to JOIN (attract -> lobby -> play), not to keep
 * playing, so a close player whose hips leave the frame is not ended while
 * the tracker still follows their shoulders. Both counts are kept per pose,
 * so switching `need_torso` between ticks reads the other rule's own window.
 * ponytail: calibration knobs, tune on glass.
 */
#define TR_PRESENT_WIN 15 /* poses, ~0.5 s at the HP's ~30 Hz */
#define TR_PRESENT_MIN 10

typedef struct {
	uint16_t torso;    /* bit k: the k-th newest pose had a torso */
	uint16_t any;      /* bit k: the k-th newest pose had a confident box */
	uint32_t last_seq; /* seq of the newest counted pose */
} tr_presence_t;

void tr_presence_init(tr_presence_t *p);
bool tr_presence_step(tr_presence_t *p, tr_box_t b, bool need_torso);
/* The verdict over the current window, without counting a pose. */
bool tr_presence_is(const tr_presence_t *p, bool need_torso);

typedef struct {
	tr_box_t ref;
	uint8_t  frames;
} tr_still_t;

void tr_still_reset(tr_still_t *s);

/* Feed one box per frame; true once the player has held still for
 * TR_STILL_FRAMES frames -- the moment to call tr_track_calibrate(). Any
 * invalid/low-confidence box or a move beyond the tolerance restarts the
 * count from that box. */
bool tr_still_step(tr_still_t *s, tr_box_t b);

#endif /* TR_POSE_H */
