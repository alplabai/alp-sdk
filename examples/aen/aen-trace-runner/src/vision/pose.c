/* src/vision/pose.c */
#include "pose.h"

#include <stdlib.h>

#include "cam_rot.h" /* TR_CAM_MIRROR */

static bool ok(const tr_pose_t *p, int k)
{
	return p->kp[k].score >= TR_POSE_KP_MIN;
}

/* Mean of the confident ones of the pair (a, a + 1): how many there were. */
static int mid(const tr_pose_t *p, int a, int *x, int *y, unsigned *conf)
{
	int k = 0, sx = 0, sy = 0;

	for (int i = a; i <= a + 1; i++) {
		if (ok(p, i)) {
			sx += p->kp[i].x;
			sy += p->kp[i].y;
			*conf += p->kp[i].score;
			k++;
		}
	}
	if (k > 0) {
		*x = sx / k;
		*y = sy / k;
	}
	return k;
}

/* Raise level of one arm: how far the wrist is above its OWN shoulder, in %
 * of the shoulder width `sw` (px). Image y grows downward, so "above" is
 * shoulder y minus wrist y; TR_CAM_FLIP_Y (track.h) turns the image over. */
static int16_t arm_raise(const tr_pose_t *p, int sho, int wri, int sw)
{
	if (p->kp[sho].score < TR_ARM_KP_MIN || p->kp[wri].score < TR_ARM_KP_MIN) {
		return TR_ARM_UNKNOWN;
	}
	int up = TR_CAM_FLIP_Y ? p->kp[wri].y - p->kp[sho].y : p->kp[sho].y - p->kp[wri].y;
	int v  = up * 100 / sw;

	return (int16_t)(v > 1000 ? 1000 : v < -1000 ? -1000 : v);
}

/* Fill b->arm_raise[] (track.h): [TR_ARM_LEFT] is the PLAYER's left arm.
 *
 * Which keypoint pair is that? Not decided by the COCO label. MoveNet labels a
 * limb by the anatomy it SEES: on any frontal figure its "left" keypoints are
 * the ones at larger image x, mirrored frame or not (a mirrored frontal
 * person is indistinguishable from an ordinary one). What is fixed is the
 * view: with TR_CAM_MIRROR the player looks at a selfie, so the arm on the
 * screen's left is their LEFT arm; without it the camera sees them face to
 * face, so the arm on the screen's left is their RIGHT. So the shoulders'
 * x order says which pair is on the screen's left, and the mirror (the
 * HP's published truth on the HE, TR_CAM_MIRROR otherwise) says whose arm
 * that is. The same rule holds at every TR_CAM_ROTATE: the pose is
 * already in the UPRIGHT frame (cam_rot.h), where the mirror is a
 * left/right one at 0, 90 and 270 alike. */
static void pose_arms(const tr_pose_t *p, bool mirrored, int16_t raise[2])
{
	raise[TR_ARM_LEFT] = raise[TR_ARM_RIGHT] = TR_ARM_UNKNOWN;
	if (p->kp[TR_KP_LSHO].score < TR_ARM_KP_MIN || p->kp[TR_KP_RSHO].score < TR_ARM_KP_MIN) {
		return;
	}
	int sw = abs(p->kp[TR_KP_LSHO].x - p->kp[TR_KP_RSHO].x);

	if (sw < TR_ARM_MIN_SHOULDER_PX) {
		return; /* side-on: no left or right to tell */
	}
	/* true: the label-LEFT pair is the one on the screen's left. */
	bool l_on_screen_left = p->kp[TR_KP_LSHO].x < p->kp[TR_KP_RSHO].x;
	/* The player's left arm is the screen-left pair when mirrored. A camera mounted
	 * upside down (TR_CAM_FLIP_Y: a 180 degree turn) reverses x as well as y, so it
	 * swaps the sides once more. */
	bool l_is_player_left = (l_on_screen_left == mirrored) != (TR_CAM_FLIP_Y != 0);

	raise[l_is_player_left ? TR_ARM_LEFT : TR_ARM_RIGHT] = arm_raise(p, TR_KP_LSHO, TR_KP_LWRI, sw);
	raise[l_is_player_left ? TR_ARM_RIGHT : TR_ARM_LEFT] = arm_raise(p, TR_KP_RSHO, TR_KP_RWRI, sw);
}

tr_box_t tr_pose_box(const tr_pose_t *p)
{
	return tr_pose_box_mirrored(p, TR_CAM_MIRROR != 0);
}

tr_box_t tr_pose_box_mirrored(const tr_pose_t *p, bool mirrored)
{
	tr_box_t b  = { .valid = false };
	int      sx = 0, sy = 0, hx = 0, hy = 0;
	unsigned conf = 0u;
	int      ns   = mid(p, TR_KP_LSHO, &sx, &sy, &conf);
	int      nh   = mid(p, TR_KP_LHIP, &hx, &hy, &conf);

	pose_arms(p, mirrored, b.arm_raise);
	if (ns == 0) {
		return b;
	}
	b.confidence = (uint8_t)(conf / (unsigned)(ns + nh));
	if (nh > 0 && hy <= sy) {
		nh = 0; /* hips above the shoulders: not a torso */
	}

	/* Lane x from the complete pairs only: a lone shoulder or hip sits a
	 * quarter-width off the body's centre. */
	int cx = (ns == 2 && nh == 2) ? (sx + hx) / 2 : (nh == 2 || ns < 2) && nh > 0 ? hx : sx;
	int sw = ns == 2 ? abs(p->kp[TR_KP_LSHO].x - p->kp[TR_KP_RSHO].x) : 0;

	b.w     = (int16_t)sw;
	b.x     = (int16_t)(cx - sw / 2);
	b.y     = (int16_t)sy;
	b.h     = (int16_t)(nh > 0 ? hy - sy : 0);
	b.valid = true;
	return b;
}

void tr_still_reset(tr_still_t *s)
{
	s->frames = 0u;
}

static int16_t absdiff(int a, int b)
{
	return (int16_t)(a > b ? a - b : b - a);
}

bool tr_still_step(tr_still_t *s, tr_box_t b)
{
	if (!b.valid || b.confidence < TR_TRACK_MIN_CONF) {
		s->frames = 0u;
		return false;
	}

	int tol = s->ref.h * TR_STILL_TOL_PCT / 100;

	if (s->frames == 0u || absdiff(b.x + b.w / 2, s->ref.x + s->ref.w / 2) > tol ||
	    absdiff(b.y, s->ref.y) > tol || absdiff(b.h, s->ref.h) > tol) {
		s->ref =
		    b; /* the anchor is the first box of the run, so a slow drift cannot creep past the tolerance */
		s->frames = 1u;
		return false;
	}
	if (s->frames < UINT8_MAX) {
		s->frames++;
	}
	return s->frames >= TR_STILL_FRAMES;
}

_Static_assert(TR_PRESENT_WIN <= 16 && TR_PRESENT_MIN <= TR_PRESENT_WIN,
               "tr_presence_t.hist holds 16 poses");

void tr_presence_init(tr_presence_t *p)
{
	*p = (tr_presence_t){ 0 };
}

static uint16_t shift_in(uint16_t hist, bool bit)
{
	return (uint16_t)(((uint32_t)hist << 1 | bit) & ((1u << TR_PRESENT_WIN) - 1u));
}

bool tr_presence_is(const tr_presence_t *p, bool need_torso)
{
	int n = 0;

	for (uint16_t h = need_torso ? p->torso : p->any; h != 0u; h &= (uint16_t)(h - 1u)) {
		n++;
	}
	return n >= TR_PRESENT_MIN;
}

bool tr_presence_step(tr_presence_t *p, tr_box_t b, bool need_torso)
{
	/* Valid or not: capture_box() tags an absent pose with its seq too,
	 * and it must count once per pose like a torso does. */
	if (b.seq != 0u && b.seq == p->last_seq) {
		return tr_presence_is(p, need_torso); /* the same pose re-read: counted once */
	}
	p->last_seq = b.seq;

	bool any = b.valid && b.confidence >= TR_TRACK_MIN_CONF;

	p->any   = shift_in(p->any, any);
	p->torso = shift_in(p->torso, any && b.h > 0);
	return tr_presence_is(p, need_torso);
}
