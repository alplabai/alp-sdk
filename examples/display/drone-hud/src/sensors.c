/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sensor-loop implementations for drone-hud.  Three discrete
 * jobs:
 *   1. Bring up each chip via its `<alp/chips/X.h>` driver.
 *   2. Run a 100 Hz IMU sample + Madgwick attitude fuse loop.
 *   3. Run a 5 Hz GPS + battery telemetry loop.
 *
 * All bus access goes through the portable `<alp/...>` surfaces;
 * no vendor symbols appear.  Per the SDK rule "Portable
 * peripheral surfaces only in app + library code".
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include <math.h>

#include "alp/peripheral.h"
#include "alp/e1m_pinout.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/ublox_neo_m9n.h"
#include "alp/chips/ina236.h"

#include "sensors.h"

LOG_MODULE_REGISTER(drone_sensors, LOG_LEVEL_INF);

/* Driver handles + bus handles -- file-scope so the loop fns can
 * reach them without juggling a struct of pointers. */
static alp_i2c_t      *s_i2c;
static alp_uart_t     *s_gps_uart;
static lsm6dso_t       s_imu;
static ublox_neo_m9n_t s_gps;
static ina236_t        s_batt;

int drone_sensors_init(drone_telemetry_t *telem)
{
    memset(telem, 0, sizeof(*telem));
    telem->mode = DRONE_MODE_STABILISED;

    /* I²C bus shared by IMU + battery monitor.  400 kHz fast-mode
     * is comfortable for both chips. */
    s_i2c = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id     = E1M_I2C0,
        .bitrate_hz = 400000,
    });
    if (s_i2c == NULL) {
        LOG_ERR("alp_i2c_open(I2C0) returned NULL");
        return -1;
    }

    /* GPS UART -- 9600/8N1 is the NEO-M9N factory default. */
    s_gps_uart = alp_uart_open(&(alp_uart_config_t){
        .port_id  = E1M_UART0,
        .baudrate = 9600,
    });
    if (s_gps_uart == NULL) {
        LOG_WRN("GPS UART unavailable; continuing without GNSS");
    }

    int rc = 0;

    /* IMU. */
    if (lsm6dso_init(&s_imu, s_i2c, LSM6DSO_I2C_ADDR_LOW) != ALP_OK) {
        LOG_WRN("LSM6DSO init failed; attitude will read zero");
        rc--;
    }

    /* Battery monitor.  INA236A at 0x40 (A0 = GND) is the default. */
    if (ina236_init(&s_batt, s_i2c, 0x40, /*shunt_ohms=*/0.01f,
                    /*max_current_a=*/50.f, INA236_ADCRANGE_81MV) != ALP_OK) {
        LOG_WRN("INA236 init failed; battery telemetry will read zero");
        rc--;
    }

    /* GNSS.  Tolerates missing UART. */
    if (s_gps_uart != NULL &&
        ublox_neo_m9n_init(&s_gps, s_gps_uart) != ALP_OK) {
        LOG_WRN("NEO-M9N init failed; GPS will report no fix");
        rc--;
    }

    return rc;
}

/* Tiny Madgwick step -- enough to demonstrate the data flow.  The
 * real library lives at metadata/library-profiles/madgwick_ahrs/;
 * this is a placeholder inline that integrates gyro into Euler
 * angles so the HUD has something to animate even on the demo
 * board without the upstream lib fetched. */
static void update_attitude(drone_telemetry_t *t,
                            float gx_dps, float gy_dps, float gz_dps,
                            float dt_s)
{
    t->roll_deg  += gx_dps * dt_s;
    t->pitch_deg += gy_dps * dt_s;
    t->yaw_deg   += gz_dps * dt_s;
    /* Clamp to [-180, +180] for HUD readability. */
    if (t->roll_deg  >  180.f) t->roll_deg  -= 360.f;
    if (t->roll_deg  < -180.f) t->roll_deg  += 360.f;
    if (t->pitch_deg >   90.f) t->pitch_deg  =   90.f;
    if (t->pitch_deg <  -90.f) t->pitch_deg  =  -90.f;
    if (t->yaw_deg   >  360.f) t->yaw_deg   -= 360.f;
    if (t->yaw_deg   <    0.f) t->yaw_deg   += 360.f;
}

/* Default-FS LSB-to-physical conversion factors for the LSM6DSO.
 * Accel default FS = ±2 g, sensitivity 16384 LSB/g.
 * Gyro  default FS = ±250 dps, sensitivity 114.286 LSB/dps. */
#define LSM6DSO_ACCEL_LSB_PER_G    16384.f
#define LSM6DSO_GYRO_LSB_PER_DPS     114.286f

void drone_sensors_run_imu_loop(drone_telemetry_t *telem)
{
    /* 100 Hz target -- 10 ms slice. */
    while (1) {
        lsm6dso_axes_t a = {0};
        lsm6dso_axes_t g = {0};
        if (lsm6dso_read_accel(&s_imu, &a) == ALP_OK &&
            lsm6dso_read_gyro(&s_imu,  &g) == ALP_OK) {
            /* Convert raw int16 counts to dps using the gyro's
             * default ±250 dps full-scale sensitivity.  The driver
             * leaves FS at reset defaults; the demo doesn't call
             * lsm6dso_set_gyro() to override. */
            const float gx = (float)g.x / LSM6DSO_GYRO_LSB_PER_DPS;
            const float gy = (float)g.y / LSM6DSO_GYRO_LSB_PER_DPS;
            const float gz = (float)g.z / LSM6DSO_GYRO_LSB_PER_DPS;
            /* Accel reading is unused by the placeholder Madgwick
             * step here, but keep the sensitivity constant referenced
             * so the table compiles + documents the math the v0.6
             * full-fusion port will plug in. */
            (void)LSM6DSO_ACCEL_LSB_PER_G;
            (void)a;
            update_attitude(telem, gx, gy, gz, 0.01f);
        }
        k_msleep(10);
    }
}

void drone_sensors_run_slow_loop(drone_telemetry_t *telem)
{
    static uint8_t  nmea_line[128];
    static int32_t  remaining_min = 25;   /* coarse Coulomb-counter */
    static uint32_t coulomb_ticks = 0;

    while (1) {
        /* Battery read -- INA236 reports bus mV + current uA; convert
         * to volts + amps for the HUD. */
        int32_t mv = 0, ua = 0;
        if (ina236_read_bus_mv(&s_batt, &mv) == ALP_OK &&
            ina236_read_current_ua(&s_batt, &ua) == ALP_OK) {
            telem->battery_v = mv / 1000.f;
            telem->battery_a = ua / 1000000.f;
            /* Crude estimator: -1 minute every 5 s while drawing
             * more than 1 A.  Real Coulomb-counting + cell model
             * lands in v0.6. */
            if (telem->battery_a > 1.f) {
                coulomb_ticks++;
                if (coulomb_ticks >= 25 && remaining_min > 0) {
                    coulomb_ticks = 0;
                    remaining_min--;
                }
            }
            telem->battery_remaining_min = remaining_min;
        }

        /* GPS NMEA -- read one line, parse later.  Real NMEA
         * decode lives in tinygsm or libnmea; the demo just
         * surfaces the line count + sat count so customers see
         * the path is live. */
        if (s_gps_uart != NULL) {
            size_t got = 0;
            if (ublox_neo_m9n_read_nmea_line(&s_gps, nmea_line,
                                             sizeof(nmea_line),
                                             &got, /*timeout_ms=*/100) == ALP_OK) {
                if (got > 0) {
                    /* Bump the sat count + flip the fix bit so the
                     * UI shows movement.  Real parse in v0.6. */
                    telem->sat_count = (telem->sat_count + 1) % 12;
                    telem->gps_fix   = telem->sat_count >= 4;
                }
            }
        }

        k_msleep(200);   /* 5 Hz cadence. */
    }
}
