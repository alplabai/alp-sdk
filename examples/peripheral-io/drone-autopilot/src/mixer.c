/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * mixer -- X-quad motor mixing for the drone-autopilot example.
 *
 * Final stage of the 1 kHz rate loop (see autopilot.c). The rate PID
 * cascade produces four scalar demands in the airframe's body frame:
 *
 *   thr    collective throttle  (0..1, common to all four motors)
 *   tau_p  roll  torque demand  (+ = roll right)
 *   tau_q  pitch torque demand  (+ = pitch up)
 *   tau_r  yaw   torque demand  (+ = yaw clockwise viewed from above)
 *
 * mixer_x_quad() distributes those onto the four ESC channels. There is
 * no hardware here -- it is pure float math, so it runs identically on
 * every E1M-family SoM and on native_sim; autopilot_emit_motors() is
 * what later pushes the result out as PWM pulse widths.
 *
 * WHY the per-motor signs differ: on an X-frame each rotor sits at a
 * corner, so its lever arm fixes how it contributes to each axis --
 *   - roll : right-side motors subtract, left-side add (tilt about X)
 *   - pitch: front motors add, rear motors subtract (tilt about Y)
 *   - yaw : comes from prop REACTION torque, so CW and CCW rotors push
 *           opposite ways -- which is exactly why adjacent motors must
 *           spin in opposite directions on every multirotor.
 * Each motor command is the sum of its four signed terms, then clipped
 * to the deliverable [0,1] band (a saturated motor simply cannot give
 * any more control authority).
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
