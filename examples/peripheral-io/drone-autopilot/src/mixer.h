/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * X-quad motor mixer.
 *
 *   M1 (CW)  ┃   ┃  M2 (CCW)         (front, viewed from above)
 *           ◢ ┃ ◣
 *           ▒▒▒▒▒
 *           ◣ ┃ ◢
 *   M4 (CCW) ┃   ┃  M3 (CW)
 *
 *   throttle increases every motor.
 *   roll  (tau_p): right wing down  -> M2/M3 up, M1/M4 down.
 *   pitch (tau_q): nose up           -> M3/M4 up, M1/M2 down.
 *   yaw   (tau_r): nose right        -> CCW motors up, CW down.
 */

#ifndef DRONE_AUTOPILOT_MIXER_H
#define DRONE_AUTOPILOT_MIXER_H

#ifdef __cplusplus
extern "C" {
#endif

/** Map throttle + 3-axis torque commands into the 4 motor outputs.
 *  @param thr   Base throttle, 0..1.
 *  @param tau_p Roll torque, -1..+1.
 *  @param tau_q Pitch torque, -1..+1.
 *  @param tau_r Yaw torque, -1..+1.
 *  @param out   Output array of 4 motor commands, 0..1. */
void mixer_x_quad(float thr, float tau_p, float tau_q, float tau_r, float out[4]);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRONE_AUTOPILOT_MIXER_H */
