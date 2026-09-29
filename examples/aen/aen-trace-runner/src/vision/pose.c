/* src/vision/pose.c */
#include "pose.h"

#include <stdlib.h>

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

tr_box_t tr_pose_box(const tr_pose_t *p)
{
	tr_box_t b = { .valid = false };
	int      sx = 0, sy = 0, hx = 0, hy = 0;
	unsigned conf = 0u;
	int      ns   = mid(p, TR_KP_LSHO, &sx, &sy, &conf);
	int      nh   = mid(p, TR_KP_LHIP, &hx, &hy, &conf);

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
		s->ref    = b; /* the anchor is the first box of the run, so a slow drift cannot creep past the tolerance */
		s->frames = 1u;
		return false;
	}
	if (s->frames < UINT8_MAX) {
		s->frames++;
	}
	return s->frames >= TR_STILL_FRAMES;
}

_Static_assert(TR_PRESENT_WIN <= 16 && TR_PRESENT_MIN <= TR_PRESENT_WIN, "tr_presence_t.hist holds 16 poses");

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
