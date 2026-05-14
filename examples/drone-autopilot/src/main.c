/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drone autopilot main entry point.
 *
 * Spawns three deterministic control loops + the RC receiver
 * thread.  The PWM bank is owned by autopilot.c; this file just
 * orchestrates the threads.
 *
 * ── Thread priorities ──────────────────────────────────────────
 *
 *   prio 1   rate_loop      1 kHz   -- highest; never blocked.
 *   prio 2   attitude_loop  250 Hz
 *   prio 3   rc_loop        50 Hz   -- SBUS UART read.
 *   prio 4   nav_loop       25 Hz   -- GPS + baro + battery.
 *   prio 5   mav_tx         10 Hz   -- MAVLink telemetry pack/send.
 *   prio 5   mav_rx         --      -- MAVLink frame parse (UART read).
 *   prio 6   main           --      -- telemetry status print.
 *
 * The rate loop owns the motors.  If it stops getting CPU time
 * the failsafe in nav_loop will cut throttle on the next 40 ms
 * tick -- the design intent is rate loop never starves but
 * defence-in-depth catches the rare case it does.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "alp/peripheral.h"

#include "autopilot.h"
#include "sbus.h"
#include "mavlink.h"

LOG_MODULE_REGISTER(drone_autopilot, LOG_LEVEL_WRN);

static autopilot_state_t g_state;

#define STACK_SIZE 4096
K_THREAD_STACK_DEFINE(stk_rate,    STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_atti,    STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_nav,     STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_rc,      STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_mav_tx,  STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_mav_rx,  STACK_SIZE);

static struct k_thread t_rate, t_atti, t_nav, t_rc, t_mav_tx, t_mav_rx;

static void rate_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    autopilot_rate_loop(&g_state);
}
static void atti_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    autopilot_attitude_loop(&g_state);
}
static void nav_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    autopilot_nav_loop(&g_state);
}
static void rc_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    autopilot_rc_loop(&g_state);
}
static void mav_tx_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    alp_mavlink_telem_loop(&g_state);
}
static void mav_rx_entry(void *p1, void *p2, void *p3) {
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    alp_mavlink_rx_loop(&g_state);
}

int main(void)
{
    LOG_INF("drone-autopilot booting (state machine in DISARMED)");

    if (autopilot_init(&g_state) != 0) {
        LOG_ERR("autopilot_init failed -- staying in DISARMED");
        /* Don't return -- still spawn the RC loop so the pilot can
         * see the arming-rejected feedback via the radio's RX-loss
         * indicator. */
    }

    k_thread_create(&t_rate, stk_rate, K_THREAD_STACK_SIZEOF(stk_rate),
                    rate_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
    k_thread_name_set(&t_rate, "rate_loop");

    k_thread_create(&t_atti, stk_atti, K_THREAD_STACK_SIZEOF(stk_atti),
                    atti_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
    k_thread_name_set(&t_atti, "atti_loop");

    k_thread_create(&t_rc, stk_rc, K_THREAD_STACK_SIZEOF(stk_rc),
                    rc_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(3), 0, K_NO_WAIT);
    k_thread_name_set(&t_rc, "rc_recv");

    k_thread_create(&t_nav, stk_nav, K_THREAD_STACK_SIZEOF(stk_nav),
                    nav_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
    k_thread_name_set(&t_nav, "nav_loop");

    /* MAVLink ground-station link -- sysid=1, compid=1.  Customers
     * tweak per-vehicle in a multi-drone swarm so QGroundControl can
     * tell them apart. */
    if (alp_mavlink_init(/*sysid=*/1, /*compid=*/1) != 0) {
        LOG_WRN("alp_mavlink_init failed -- telemetry link disabled");
    } else {
        k_thread_create(&t_mav_tx, stk_mav_tx,
                        K_THREAD_STACK_SIZEOF(stk_mav_tx),
                        mav_tx_entry, NULL, NULL, NULL,
                        K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
        k_thread_name_set(&t_mav_tx, "mav_tx");

        k_thread_create(&t_mav_rx, stk_mav_rx,
                        K_THREAD_STACK_SIZEOF(stk_mav_rx),
                        mav_rx_entry, NULL, NULL, NULL,
                        K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
        k_thread_name_set(&t_mav_rx, "mav_rx");
    }

    /* Slow telemetry print -- 1 Hz status line over the diagnostic
     * UART.  Useful when no GCS is attached. */
    while (1) {
        LOG_INF("mode=%d arm=%d roll=%.1f pitch=%.1f alt=%.1fm bat=%.2fV",
                (int)g_state.mode, (int)g_state.armed,
                (double)g_state.roll, (double)g_state.pitch,
                (double)g_state.altitude_m, (double)g_state.battery_v);
        k_msleep(1000);
    }
    return 0;
}

/* ── RC receiver loop ─────────────────────────────────────────── */

extern alp_uart_t *autopilot_rc_uart(void);   /* defined in autopilot.c */

void autopilot_rc_loop(autopilot_state_t *s)
{
    /* The autopilot owns the UART handle; we pull frame bytes via
     * a small re-export to keep this file pure-orchestration. */
    uint32_t last_frame_ms = 0;
    while (1) {
        sbus_frame_t f;
        /* TODO(sbus): real UART byte-stream framing -- find the
         * 0x0F start byte, then read 24 bytes.  Stubbed here so
         * the demo compiles standalone; v0.6 wires the real
         * frame slicer.  Pretend we got a frame every 20 ms so
         * the failsafe doesn't trip in the simulator. */
        if (true) {
            for (int i = 0; i < SBUS_CHANNELS; i++) f.channel[i] = 1024;
            f.frame_lost = false;
            f.failsafe   = false;
            last_frame_ms = k_uptime_get_32();
        }

        /* Channel mapping (Futaba convention):
         *   ch1=roll  ch2=pitch  ch3=throttle  ch4=yaw  ch5=mode */
        s->stick_roll     = ((int)f.channel[0] - 1024) / 800.f;
        s->stick_pitch    = ((int)f.channel[1] - 1024) / 800.f;
        s->stick_throttle = (f.channel[2]) / 2048.f;
        s->stick_yaw      = ((int)f.channel[3] - 1024) / 800.f;

        /* Mode switch: ch5 below 600 -> MANUAL, 600..1400 -> STABILISE,
         * above 1400 -> GPS-HOLD.  Edge cases roll up to RTH via the
         * dedicated ch6 trip. */
        if      (f.channel[4] < 600)  s->mode = AP_MODE_MANUAL;
        else if (f.channel[4] < 1400) s->mode = AP_MODE_STABILISE;
        else                          s->mode = AP_MODE_GPS_HOLD;
        if (f.channel[5] > 1400)      s->mode = AP_MODE_RTH;

        /* Arming -- ch7 high + throttle low. */
        s->armed = (f.channel[6] > 1400) && (s->stick_throttle < 0.05f);
        if (!s->armed) s->mode = AP_MODE_DISARMED;

        /* RC link-OK if we've seen a frame in the last 200 ms. */
        s->rc_link_ok = (k_uptime_get_32() - last_frame_ms) < 200;

        k_msleep(20);
    }
}
