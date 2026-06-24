/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drone autopilot -- cascaded PID rate + attitude + altitude loops,
 * flight-mode state machine, motor mixer.  Single-thread per loop;
 * data flows via the shared autopilot_state_t snapshot.
 */

#ifndef DRONE_AUTOPILOT_H
#define DRONE_AUTOPILOT_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define AUTOPILOT_N_MOTORS 4 /* X-quad layout. */

/** Flight-mode state machine.  Pilot switches via RC channel 5;
 *  failsafe is the only state the autopilot enters on its own. */
typedef enum {
	AP_MODE_DISARMED  = 0, /**< Motors off; throttle locked to 0. */
	AP_MODE_MANUAL    = 1, /**< Direct rate-stick → motor output. */
	AP_MODE_STABILISE = 2, /**< Attitude-stabilised; pilot has stick authority. */
	AP_MODE_GPS_HOLD  = 3, /**< Position + altitude hold. */
	AP_MODE_RTH       = 4, /**< Return to home -- waypoint nav back to launch GPS fix. */
	AP_MODE_FAILSAFE  = 5, /**< RC link lost / battery critical -- descend + land. */
} ap_mode_t;

/** Shared autopilot state.  Single-writer per field:
 *    - rate / atti / nav loops write their own outputs.
 *    - SBUS thread writes stick + mode.
 *    - main thread reads everything for telemetry / motor output.
 *  Memcpy-snapshot read pattern keeps frames consistent without
 *  locks. */
typedef struct {
	/* Attitude (Madgwick output, ENU frame, degrees). */
	float roll, pitch, yaw;

	/* Angular rates (gyro, rad/s). */
	float p, q, r;

	/* Altitude estimate (m AGL, from barometer + GNSS fusion). */
	float altitude_m;
	float climb_rate_mps;

	/* GNSS. */
	float lat_deg, lon_deg;
	bool  gps_fix;

	/* Battery. */
	float battery_v;
	float battery_a;

	/* RC stick inputs (normalised −1..+1; throttle 0..1). */
	float stick_throttle;
	float stick_roll;
	float stick_pitch;
	float stick_yaw;
	bool  rc_link_ok;

	/* Setpoints (mode-dependent: stick-driven in MANUAL/STABILISE,
     * planner-driven in GPS-HOLD/RTH). */
	float setp_roll, setp_pitch, setp_yaw;
	float setp_altitude_m;

	/* Motor commands (0..1 throttle scale). */
	float motor_cmd[AUTOPILOT_N_MOTORS];

	/* Flight mode. */
	ap_mode_t mode;
	bool      armed;
} autopilot_state_t;

/** Bring up sensors + PWMs + UARTs.  @return 0 on success. */
int autopilot_init(autopilot_state_t *s);

/** 1 kHz rate loop -- PID on body rates → motor mixer → ESCs.
 *  Runs forever; spawn as its own thread. */
void autopilot_rate_loop(autopilot_state_t *s);

/** 250 Hz attitude loop -- IMU sample → Madgwick fuse → attitude
 *  PID → rate setpoints feed forward into the rate loop. */
void autopilot_attitude_loop(autopilot_state_t *s);

/** 25 Hz navigation loop -- GNSS + barometer → altitude/position
 *  PID → attitude setpoints feed forward into the attitude loop. */
void autopilot_nav_loop(autopilot_state_t *s);

/** SBUS RC receiver parse thread -- 50 Hz UART poll, decode 16
 *  channels into the stick + mode fields. */
void autopilot_rc_loop(autopilot_state_t *s);

/** Push motor_cmd[] to the ESC PWM channels.  Called by the rate
 *  loop after each PID iteration. */
void autopilot_emit_motors(const autopilot_state_t *s);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRONE_AUTOPILOT_H */
