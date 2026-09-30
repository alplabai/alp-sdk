/* src/vision/kp_smooth.h -- temporal keypoint smoothing on the HP, before
 * the pose is published: a One-Euro filter (Casiez, Roussel, Vogel, CHI
 * 2012) per keypoint coordinate. A low-pass whose cutoff rises with the
 * coordinate's own filtered speed: a player standing still gets heavy
 * smoothing (MoveNet Lightning's frame-to-frame jitter), a jump or a lane
 * step gets almost none (the intent still registers within 2 frames --
 * tests/host/test_kp_smooth.c).
 *
 * A keypoint below TR_POSE_KP_MIN is HELD at its last smoothed position
 * (its own low score still published, so every consumer keeps ignoring it)
 * instead of jumping to wherever an unsure decode put it; one held for more
 * than TR_KS_HOLD_MAX frames re-seeds from the raw value when it comes back.
 *
 * Pure C, cheap float (34 channels, one divide each: microseconds on the
 * M55-HP). Upright frame px (cam_rot.h), dt from the HP's own frame clock.
 */
#ifndef TR_KP_SMOOTH_H
#define TR_KP_SMOOTH_H

#include <stdbool.h>
#include <stdint.h>

#include "pose.h"

/* ponytail: calibration knobs, tune on glass. fc_min: cutoff at rest, Hz
 * (lower = smoother standing still). beta: cutoff gain per px/s of speed
 * (higher = less lag moving). d_cutoff: the speed estimate's own cutoff, Hz. */
#define TR_KS_FC_MIN   1.0f
#define TR_KS_BETA     0.03f
#define TR_KS_D_CUTOFF 2.0f
#define TR_KS_HOLD_MAX 10  /* frames a low-confidence keypoint is held before a re-seed */
#define TR_KS_DT_MAX   250 /* ms: a longer gap (stalled stream) re-seeds every keypoint */

typedef struct {
	float x, dx; /* filtered value, filtered speed (px/s) */
} tr_oe_t;

typedef struct {
	tr_oe_t ch[TR_POSE_KP][2]; /* x, y */
	uint8_t held[TR_POSE_KP];  /* consecutive frames below TR_POSE_KP_MIN */
	bool    seeded[TR_POSE_KP];
} tr_kp_smooth_t;

void tr_kp_smooth_reset(tr_kp_smooth_t *s);

/* Smooth p in place; dt_ms since the previous call (the frame interval). */
void tr_kp_smooth_step(tr_kp_smooth_t *s, tr_pose_t *p, uint32_t dt_ms);

#endif /* TR_KP_SMOOTH_H */
