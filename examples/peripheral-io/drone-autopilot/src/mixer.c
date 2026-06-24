/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 */

#include "mixer.h"

static inline float clip01(float v)
{
	if (v < 0.f) return 0.f;
	if (v > 1.f) return 1.f;
	return v;
}

void mixer_x_quad(float thr, float tau_p, float tau_q, float tau_r, float out[4])
{
	/* X-quad with M1 front-right (CW), M2 front-left (CCW),
     * M3 rear-left (CW), M4 rear-right (CCW). */
	out[0] = clip01(thr - tau_p + tau_q - tau_r); /* M1 FR CW */
	out[1] = clip01(thr + tau_p + tau_q + tau_r); /* M2 FL CCW */
	out[2] = clip01(thr + tau_p - tau_q - tau_r); /* M3 RL CW */
	out[3] = clip01(thr - tau_p - tau_q + tau_r); /* M4 RR CCW */
}
