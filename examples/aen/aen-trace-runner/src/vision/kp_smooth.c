/* src/vision/kp_smooth.c -- see kp_smooth.h. */
#include <string.h>

#include "kp_smooth.h"

#define TWO_PI 6.2831853f

/* The exponential smoothing factor for cutoff fc at step te seconds:
 * 1 / (1 + tau / te), tau = 1 / (2 pi fc) -- as r / (1 + r), r = 2 pi fc te. */
static float alpha(float fc, float te)
{
	float r = TWO_PI * fc * te;

	return r / (1.0f + r);
}

static float oe_step(tr_oe_t *f, float x, float te)
{
	float dx = (x - f->x) / te;

	f->dx     = f->dx + alpha(TR_KS_D_CUTOFF, te) * (dx - f->dx);
	float adx = f->dx > 0.0f ? f->dx : -f->dx;

	f->x = f->x + alpha(TR_KS_FC_MIN + TR_KS_BETA * adx, te) * (x - f->x);
	return f->x;
}

/* Round half away from zero: a position can sit just outside the frame
 * (the letterbox padding decodes negative). */
static int16_t px_round(float v)
{
	return (int16_t)(v < 0.0f ? v - 0.5f : v + 0.5f);
}

void tr_kp_smooth_reset(tr_kp_smooth_t *s)
{
	memset(s, 0, sizeof(*s));
}

void tr_kp_smooth_step(tr_kp_smooth_t *s, tr_pose_t *p, uint32_t dt_ms)
{
	float te = (float)(dt_ms ? dt_ms : 1u) / 1000.0f;

	if (dt_ms > TR_KS_DT_MAX) {
		tr_kp_smooth_reset(s); /* the stream stalled: nothing to be continuous with */
	}
	for (int k = 0; k < TR_POSE_KP; k++) {
		tr_kp_t *kp = &p->kp[k];

		if (kp->score < TR_POSE_KP_MIN) {
			/* unsure: hold the last smoothed position, never jump to the
			 * unsure decode; long enough gone, forget it */
			if (s->seeded[k] && s->held[k] < TR_KS_HOLD_MAX) {
				s->held[k]++;
				kp->x = px_round(s->ch[k][0].x);
				kp->y = px_round(s->ch[k][1].x);
			} else {
				s->seeded[k] = false;
			}
			continue;
		}
		if (!s->seeded[k]) {
			s->ch[k][0]  = (tr_oe_t){ (float)kp->x, 0.0f };
			s->ch[k][1]  = (tr_oe_t){ (float)kp->y, 0.0f };
			s->seeded[k] = true;
			s->held[k]   = 0u;
			continue; /* the first sample passes through */
		}
		s->held[k] = 0u;
		kp->x      = px_round(oe_step(&s->ch[k][0], (float)kp->x, te));
		kp->y      = px_round(oe_step(&s->ch[k][1], (float)kp->y, te));
	}
}
