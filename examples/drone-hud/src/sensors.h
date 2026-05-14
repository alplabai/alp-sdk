/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared telemetry types + sensor-loop entrypoints for drone-hud.
 *
 * Single writer / single reader per field, so the volatile-
 * snapshot read in main() is correct without locks.
 */

#ifndef DRONE_HUD_SENSORS_H
#define DRONE_HUD_SENSORS_H

#include <stdint.h>
#include <stdbool.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Live drone telemetry snapshot.  Updated by the IMU + slow
 *  telemetry threads; read once-per-frame by the LVGL render loop. */
typedef struct {
    /* Attitude (Madgwick output, ENU frame). */
    float roll_deg;        /**< Roll about X axis. */
    float pitch_deg;       /**< Pitch about Y axis. */
    float yaw_deg;         /**< Yaw about Z axis (compass heading). */

    /* GPS. */
    float    lat_deg;
    float    lon_deg;
    float    altitude_m;
    uint8_t  sat_count;
    bool     gps_fix;

    /* Battery (INA236). */
    float    battery_v;
    float    battery_a;
    int32_t  battery_remaining_min;  /**< Coulomb-counter estimate. */

    /* Flight mode selector (host UI sets, autopilot reads). */
    enum {
        DRONE_MODE_MANUAL     = 0,
        DRONE_MODE_STABILISED = 1,
        DRONE_MODE_GPS_HOLD   = 2,
    } mode;
} drone_telemetry_t;

/** Bring up every chip + library this demo touches.  Tolerates
 *  partially-populated carriers (missing chip -> log + continue).
 *  @return 0 on full bring-up, non-zero if any chip failed. */
int drone_sensors_init(drone_telemetry_t *telem);

/** IMU thread entry -- runs forever; ~100 Hz LSM6DSO sample +
 *  Madgwick attitude fusion. */
void drone_sensors_run_imu_loop(drone_telemetry_t *telem);

/** Slow-telemetry thread entry -- runs forever; 5 Hz GPS NMEA
 *  parse + INA236 battery read. */
void drone_sensors_run_slow_loop(drone_telemetry_t *telem);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* DRONE_HUD_SENSORS_H */
