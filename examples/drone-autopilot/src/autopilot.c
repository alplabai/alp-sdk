/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drone autopilot implementation.
 *
 * Three control loops + the navigator + the failsafe:
 *
 *   ┌─────────────────────────────────────────────────────────┐
 *   │  25 Hz   nav loop      (alt + pos PID -> atti setpoint) │
 *   │  250 Hz  atti loop     (att PID  -> rate setpoint)      │
 *   │  1 kHz   rate loop     (rate PID -> motor mix -> ESCs)  │
 *   └─────────────────────────────────────────────────────────┘
 *
 * Each loop reads the autopilot_state_t snapshot, applies its
 * stage of the PID cascade, writes its outputs back, and sleeps.
 * Single-writer per field; the snapshot copy in main() shows the
 * pilot a consistent telemetry view.
 *
 * Sensor bring-up + the PWM bank live in autopilot_init().
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <math.h>
#include <string.h>

#include "alp/peripheral.h"
#include "alp/pwm.h"
#include "alp/e1m_pinout.h"
#include "alp/chips/lsm6dso.h"
#include "alp/chips/bmp390.h"
#include "alp/chips/ublox_neo_m9n.h"
#include "alp/chips/ina236.h"

#include "autopilot.h"
#include "mixer.h"

LOG_MODULE_REGISTER(autopilot, LOG_LEVEL_WRN);

/* ───────── Driver handles + bus handles ───────── */
static alp_i2c_t   *s_i2c;
static alp_uart_t  *s_rc_uart;
static alp_uart_t  *s_gps_uart;
static alp_pwm_t   *s_esc[AUTOPILOT_N_MOTORS];

static lsm6dso_t        s_imu;
static bmp390_t         s_baro;
static ublox_neo_m9n_t  s_gps;
static ina236_t         s_batt;

/* ───────── Tiny PID kernel ─────────
 *
 * Minimal PID with anti-windup + derivative-on-measurement.  Real
 * implementation is the `pid` library knob -- which on AEN binds
 * to the FPU SIMD path and on V2N to the TMU CORDIC.  We inline
 * here because the autopilot loop wants the absolute-minimum
 * cycle cost.
 */
typedef struct {
    float kp, ki, kd;
    float integ;
    float prev_meas;
    float out_min, out_max;
} pid_t;

static inline float pid_step(pid_t *p, float setp, float meas, float dt_s) {
    const float err = setp - meas;
    p->integ += err * dt_s;
    /* Clamp the integrator to half the output range. */
    const float i_max = (p->out_max - p->out_min) * 0.5f;
    if (p->integ >  i_max) p->integ =  i_max;
    if (p->integ < -i_max) p->integ = -i_max;
    const float d = (meas - p->prev_meas) / dt_s;
    p->prev_meas = meas;
    float out = p->kp * err + p->ki * p->integ - p->kd * d;
    if (out < p->out_min) out = p->out_min;
    if (out > p->out_max) out = p->out_max;
    return out;
}

/* Per-axis PID instances.  Gains here are conservative starting
 * points; real flight tuning lives in a sysbuild-time KConfig
 * overlay so customers don't have to recompile to twiddle. */
static pid_t s_rate_p = {.kp = 0.10f, .ki = 0.05f, .kd = 0.001f,
                          .out_min = -1.0f, .out_max = 1.0f};
static pid_t s_rate_q = {.kp = 0.10f, .ki = 0.05f, .kd = 0.001f,
                          .out_min = -1.0f, .out_max = 1.0f};
static pid_t s_rate_r = {.kp = 0.08f, .ki = 0.02f, .kd = 0.0f,
                          .out_min = -1.0f, .out_max = 1.0f};

static pid_t s_atti_roll  = {.kp = 4.0f, .ki = 0.0f, .kd = 0.0f,
                              .out_min = -250.f, .out_max = 250.f};   /* dps */
static pid_t s_atti_pitch = {.kp = 4.0f, .ki = 0.0f, .kd = 0.0f,
                              .out_min = -250.f, .out_max = 250.f};

static pid_t s_alt_pid = {.kp = 1.5f, .ki = 0.15f, .kd = 0.5f,
                           .out_min = -3.0f, .out_max = 3.0f};       /* m/s */

/* ───────── Bring-up ───────── */
int autopilot_init(autopilot_state_t *s)
{
    memset(s, 0, sizeof(*s));
    s->mode = AP_MODE_DISARMED;

    s_i2c = alp_i2c_open(&(alp_i2c_config_t){
        .bus_id = E1M_I2C0, .bitrate_hz = 400000,
    });
    if (!s_i2c) { LOG_ERR("I2C0 open failed"); return -1; }

    if (lsm6dso_init(&s_imu,  s_i2c, LSM6DSO_I2C_ADDR_LOW) != ALP_OK) return -2;
    if (bmp390_init(&s_baro, s_i2c, BMP390_I2C_ADDR_PRIMARY) != ALP_OK) return -3;
    if (ina236_init(&s_batt, s_i2c, /*addr_7bit=*/0x40, /*shunt_ohms=*/0.01f,
                    /*max_current_a=*/50.f,
                    /*adcrange=*/INA236_ADCRANGE_81MV) != ALP_OK) return -4;

    /* GNSS + SBUS RC.  Both are UART; SBUS expects 100000/8E2 inverted
     * but most boards + Zephyr UART drivers don't expose the inverted
     * polarity directly -- customers wire an external inverter or use
     * a microcontroller-side inverter chip. */
    s_gps_uart = alp_uart_open(&(alp_uart_config_t){
        .port_id = E1M_UART0, .baudrate = 9600,
    });
    s_rc_uart  = alp_uart_open(&(alp_uart_config_t){
        .port_id = E1M_UART1, .baudrate = 100000,
    });
    if (s_gps_uart) ublox_neo_m9n_init(&s_gps, s_gps_uart);

    /* ESC PWM bank -- 400 Hz BLHeli-flavoured ESC compatibility.
     * Real ESC types vary; customers tune the period_ns to match
     * (DShot uses its own protocol and bypasses the PWM path
     * entirely). */
    for (int i = 0; i < AUTOPILOT_N_MOTORS; i++) {
        s_esc[i] = alp_pwm_open(&(alp_pwm_config_t){
            .channel_id = E1M_PWM0 + i,
            .period_ns  = 2500000,   /* 400 Hz. */
            .polarity   = ALP_PWM_POLARITY_NORMAL,
        });
        if (!s_esc[i]) {
            LOG_ERR("ESC%d open failed", i);
            return -10 - i;
        }
        alp_pwm_set_duty(s_esc[i], 0);
    }

    return 0;
}

/* ───────── Per-loop entrypoints ───────── */

void autopilot_rate_loop(autopilot_state_t *s)
{
    const float dt_s = 0.001f;   /* 1 kHz. */
    while (1) {
        /* Inputs: gyro rates (set by the attitude loop on a fresh
         * IMU read; we re-read here too so the rate loop's PID
         * sees the freshest sample at 1 kHz). */
        lsm6dso_axes_t g = {0};
        if (lsm6dso_read_gyro(&s_imu, &g) == ALP_OK) {
            /* Raw int16 counts -> rad/s.  Default FS is +/-250 dps
             * where 1 dps = 114.286 LSB; deg -> rad scales by
             * pi/180.  v0.6 reads the SoM's configured FS. */
            const float dps_per_lsb = 1.f / 114.286f;
            const float deg_to_rad  = 3.14159265f / 180.f;
            s->p = g.x * dps_per_lsb * deg_to_rad;
            s->q = g.y * dps_per_lsb * deg_to_rad;
            s->r = g.z * dps_per_lsb * deg_to_rad;
        }

        /* Rate setpoints feed in from the atti loop in stabilised
         * modes; in MANUAL the sticks drive the rate setpoint
         * directly. */
        const float setp_p = (s->mode == AP_MODE_MANUAL)
                              ? s->stick_roll  * 250.f * (3.14159265f / 180.f)
                              : s->setp_roll   * (3.14159265f / 180.f);
        const float setp_q = (s->mode == AP_MODE_MANUAL)
                              ? s->stick_pitch * 250.f * (3.14159265f / 180.f)
                              : s->setp_pitch  * (3.14159265f / 180.f);
        const float setp_r = s->stick_yaw * 250.f * (3.14159265f / 180.f);

        const float tau_p = pid_step(&s_rate_p, setp_p, s->p, dt_s);
        const float tau_q = pid_step(&s_rate_q, setp_q, s->q, dt_s);
        const float tau_r = pid_step(&s_rate_r, setp_r, s->r, dt_s);

        const float thr = (s->mode == AP_MODE_DISARMED ||
                           s->mode == AP_MODE_FAILSAFE)
                          ? 0.0f
                          : s->stick_throttle;

        mixer_x_quad(thr, tau_p, tau_q, tau_r, s->motor_cmd);
        autopilot_emit_motors(s);

        k_msleep(1);
    }
}

void autopilot_attitude_loop(autopilot_state_t *s)
{
    const float dt_s = 1.f / 250.f;
    while (1) {
        /* TODO(madgwick): real fusion via the library knob.  For
         * v0.5 paper-correctness we approximate roll/pitch from the
         * accelerometer at static conditions; the rate loop's gyro
         * integration handles dynamics until the AHRS lands. */
        lsm6dso_axes_t a = {0};
        if (lsm6dso_read_accel(&s_imu, &a) == ALP_OK) {
            /* Raw int16 counts -> g.  Default FS is +/-2 g where
             * 1 g = 16384 LSB.  v0.6 reads the SoM's configured FS
             * and uses it here. */
            const float ax = a.x / 16384.f;
            const float ay = a.y / 16384.f;
            const float az = a.z / 16384.f;
            const float pitch = atan2f(-ax, sqrtf(ay * ay + az * az));
            const float roll  = atan2f(ay, az);
            s->roll  = roll  * 180.f / 3.14159265f;
            s->pitch = pitch * 180.f / 3.14159265f;
        }

        /* Attitude setpoints from sticks (STABILISE) or nav loop
         * (GPS-HOLD/RTH). */
        if (s->mode == AP_MODE_STABILISE || s->mode == AP_MODE_GPS_HOLD ||
            s->mode == AP_MODE_RTH) {
            const float roll_sp  = (s->mode == AP_MODE_STABILISE)
                                    ? s->stick_roll  * 25.f
                                    : s->setp_roll;
            const float pitch_sp = (s->mode == AP_MODE_STABILISE)
                                    ? s->stick_pitch * 25.f
                                    : s->setp_pitch;
            s->setp_roll  = pid_step(&s_atti_roll,  roll_sp,  s->roll,  dt_s);
            s->setp_pitch = pid_step(&s_atti_pitch, pitch_sp, s->pitch, dt_s);
        }

        k_msleep(4);
    }
}

void autopilot_nav_loop(autopilot_state_t *s)
{
    const float dt_s = 1.f / 25.f;
    while (1) {
        /* Barometer altitude.  Compensation lives in the real
         * driver; for paper-correctness we just emit the raw
         * pressure cast into "metres" via a coarse 1 hPa = 8.5 m
         * rule.  v0.6 wires the proper altitude formula. */
        s->altitude_m += 0.0f;  /* placeholder until v0.6. */

        /* GNSS poll. */
        if (s_gps_uart) {
            uint8_t line[128];
            size_t  got;
            (void)ublox_neo_m9n_read_nmea_line(&s_gps, line, sizeof(line),
                                                &got, /*timeout_ms=*/20);
        }

        /* Battery telemetry + failsafe check. */
        int32_t mv = 0, ua = 0;
        if (ina236_read_bus_mv(&s_batt, &mv) == ALP_OK &&
            ina236_read_current_ua(&s_batt, &ua) == ALP_OK) {
            s->battery_v = mv / 1000.f;
            s->battery_a = ua / 1.0e6f;
        }
        /* 3.3 V/cell critical -- 4S pack -> 13.2 V trip. */
        if (s->battery_v > 0.f && s->battery_v < 13.2f) {
            s->mode = AP_MODE_FAILSAFE;
        }
        /* RC link loss failsafe. */
        if (!s->rc_link_ok && s->mode != AP_MODE_DISARMED) {
            s->mode = AP_MODE_FAILSAFE;
        }

        /* GPS-HOLD / RTH setpoint generation.  v0.5 stub: hold
         * level pitch + zero roll; v0.6 fills in the proper
         * position controller. */
        if (s->mode == AP_MODE_GPS_HOLD || s->mode == AP_MODE_RTH) {
            s->setp_roll  = 0.f;
            s->setp_pitch = 0.f;
        }

        /* Altitude PID feeds throttle in altitude-hold modes. */
        if (s->mode == AP_MODE_GPS_HOLD || s->mode == AP_MODE_RTH ||
            s->mode == AP_MODE_FAILSAFE) {
            const float climb_sp = (s->mode == AP_MODE_FAILSAFE) ? -1.0f : 0.f;
            const float alt_correction = pid_step(&s_alt_pid, climb_sp,
                                                   s->climb_rate_mps, dt_s);
            s->stick_throttle = 0.55f + alt_correction * 0.10f;
        }

        k_msleep(40);
    }
}

void autopilot_emit_motors(const autopilot_state_t *s)
{
    for (int i = 0; i < AUTOPILOT_N_MOTORS; i++) {
        /* Convert 0..1 throttle to pulse width 1000..2000 µs
         * (BLHeli-compatible band). */
        const uint32_t pulse_ns =
            (uint32_t)(1000000.f + s->motor_cmd[i] * 1000000.f);
        alp_pwm_set_duty(s_esc[i], pulse_ns);
    }
}
