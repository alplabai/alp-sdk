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

/* Single shared autopilot state.  Every loop reads/writes only its
 * own slice of this struct -- see autopilot.h for the single-writer
 * convention.  File-scope so the thread entry trampolines below can
 * find it without dragging context pointers through k_thread_create. */
static autopilot_state_t g_state;

/* 4 KiB per loop is generous for the cascaded PID + small ring buffers;
 * Madgwick + the mixer keep their state in g_state, not on the stack. */
#define STACK_SIZE 4096
K_THREAD_STACK_DEFINE(stk_rate,    STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_atti,    STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_nav,     STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_rc,      STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_mav_tx,  STACK_SIZE);
K_THREAD_STACK_DEFINE(stk_mav_rx,  STACK_SIZE);

/* Thread control blocks live alongside their stacks so the scheduler
 * can find them; each one binds to exactly one of the loops below. */
static struct k_thread t_rate, t_atti, t_nav, t_rc, t_mav_tx, t_mav_rx;

/* ── Thread entry trampolines ───────────────────────────────────────
 * Zephyr's k_thread_create signature is (void*,void*,void*) so each
 * loop gets a thin shim that forwards into the strongly-typed loop
 * function with the shared g_state pointer.  Keeping them tiny keeps
 * the call site obvious to the reviewer. */
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

    /* ── Sensor + actuator bring-up ────────────────────────────────
     * autopilot_init() opens the IMU (LSM6DSO over I2C), the
     * barometer (BMP390), the GPS UART (u-blox NEO-M9N), the battery
     * sense (INA236), the SBUS RX UART, and the four ESC PWMs.  All
     * of these come through the portable `<alp/*>` surfaces so the
     * same code runs across any SoM whose preset declares the bus
     * mapping; chip-specific work lives behind alp_chips/<part>.
     *
     * If init fails (a sensor missing, wrong i2c addr, …) we DO NOT
     * exit -- the autopilot stays in DISARMED forever, but we still
     * need the RC loop alive so the pilot can see the arming-rejected
     * feedback via the radio's RX-loss / failsafe indicator on their
     * transmitter.  Silent dead firmware is worse than a flashing
     * red light. */
    if (autopilot_init(&g_state) != 0) {
        LOG_ERR("autopilot_init failed -- staying in DISARMED");
        /* Don't return -- still spawn the RC loop so the pilot can
         * see the arming-rejected feedback via the radio's RX-loss
         * indicator. */
    }

    /* ── Spawn control loops in priority order ─────────────────────
     * Higher-rate loops get higher priority (lower numeric value in
     * Zephyr's PREEMPT scheme).  The rate loop at 1 kHz must never
     * be starved -- everything else is allowed to slip a tick if the
     * CPU is busy; the rate loop is the one that keeps the airframe
     * upright. */

    /* prio 1: 1 kHz inner rate loop -- gyro -> rate PID -> motor mix
     * -> ESC PWMs.  Highest priority of any application thread.  If
     * this ever stops getting CPU time, the failsafe in nav_loop
     * will cut throttle on the next 40 ms tick (defence in depth). */
    k_thread_create(&t_rate, stk_rate, K_THREAD_STACK_SIZEOF(stk_rate),
                    rate_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(1), 0, K_NO_WAIT);
    k_thread_name_set(&t_rate, "rate_loop");

    /* prio 2: 250 Hz attitude loop -- accel+gyro -> Madgwick fuse
     * -> attitude PID -> rate setpoints.  Feeds the rate loop above. */
    k_thread_create(&t_atti, stk_atti, K_THREAD_STACK_SIZEOF(stk_atti),
                    atti_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(2), 0, K_NO_WAIT);
    k_thread_name_set(&t_atti, "atti_loop");

    /* prio 3: 50 Hz SBUS RC receiver -- UART read of the 25-byte
     * SBUS frame from the radio.  Writes stick + mode + rc_link_ok
     * into g_state for the inner loops to consume. */
    k_thread_create(&t_rc, stk_rc, K_THREAD_STACK_SIZEOF(stk_rc),
                    rc_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(3), 0, K_NO_WAIT);
    k_thread_name_set(&t_rc, "rc_recv");

    /* prio 4: 25 Hz navigation loop -- GPS + baro + battery monitoring.
     * Lower frequency because GNSS solutions only refresh ~10 Hz and
     * altitude smoothing benefits from longer windows.  Also owns the
     * failsafe arbitration (RC-loss, battery-critical, etc.). */
    k_thread_create(&t_nav, stk_nav, K_THREAD_STACK_SIZEOF(stk_nav),
                    nav_entry, NULL, NULL, NULL,
                    K_PRIO_PREEMPT(4), 0, K_NO_WAIT);
    k_thread_name_set(&t_nav, "nav_loop");

    /* ── MAVLink ground-station link (optional) ────────────────────
     * QGroundControl / Mission Planner speak MAVLink over a serial
     * radio (typically a 433/915 MHz SiK telemetry modem).  sysid=1
     * + compid=1 is the convention for "the autopilot itself"; in a
     * multi-drone swarm each airframe gets a unique sysid so the GCS
     * can tell them apart.  Customers usually wire this to a SoM
     * Kconfig knob rather than hard-coding. */
    if (alp_mavlink_init(/*sysid=*/1, /*compid=*/1) != 0) {
        /* Telemetry is non-critical: a failed link just means no
         * GCS visibility.  Flight loops above keep running. */
        LOG_WRN("alp_mavlink_init failed -- telemetry link disabled");
    } else {
        /* prio 5: 10 Hz outbound telemetry -- packs HEARTBEAT,
         * ATTITUDE, GLOBAL_POSITION_INT, BATTERY_STATUS frames from
         * g_state and writes them to the GCS UART. */
        k_thread_create(&t_mav_tx, stk_mav_tx,
                        K_THREAD_STACK_SIZEOF(stk_mav_tx),
                        mav_tx_entry, NULL, NULL, NULL,
                        K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
        k_thread_name_set(&t_mav_tx, "mav_tx");

        /* prio 5: inbound MAVLink parser -- blocks in UART read,
         * dispatches MAV_CMD_* into the autopilot (e.g. arm/disarm
         * from QGC, waypoint upload, parameter set).  Same priority
         * as TX because neither is timing-critical. */
        k_thread_create(&t_mav_rx, stk_mav_rx,
                        K_THREAD_STACK_SIZEOF(stk_mav_rx),
                        mav_rx_entry, NULL, NULL, NULL,
                        K_PRIO_PREEMPT(5), 0, K_NO_WAIT);
        k_thread_name_set(&t_mav_rx, "mav_rx");
    }

    /* ── Main thread becomes the diagnostic printer ────────────────
     * Slow telemetry print at 1 Hz to the diagnostic UART (Zephyr
     * console).  Useful during bring-up on the bench when no GCS is
     * attached -- you can watch attitude + altitude + battery climb
     * via `west espresso monitor` or any TTY viewer.
     *
     * Lowest effective priority (prio 6 by virtue of being the main
     * thread Zephyr spawned).  This loop never returns; the four
     * spawned loops run forever. */
    while (1) {
        LOG_INF("mode=%d arm=%d roll=%.1f pitch=%.1f alt=%.1fm bat=%.2fV",
                (int)g_state.mode, (int)g_state.armed,
                (double)g_state.roll, (double)g_state.pitch,
                (double)g_state.altitude_m, (double)g_state.battery_v);
        k_msleep(1000);
    }
    /* Unreachable -- here so the compiler doesn't whine about main's
     * declared return type.  If you ever turn the while-loop into a
     * conditional, this is your normal exit. */
    return 0;
}

/* ── RC receiver loop ─────────────────────────────────────────────
 *
 * SBUS is the Futaba serial RC protocol used by most modern radio
 * receivers: 100 kbaud, 8E2, inverted UART, 25-byte frames at 14 ms
 * cadence (or 7 ms in "high-speed" mode).  Each frame carries 16
 * 11-bit channels plus a flags byte (frame_lost, failsafe).
 *
 * Lives in main.c rather than autopilot.c so the orchestration is
 * all in one place; autopilot.c owns the sensors+motors and exposes
 * the SBUS UART handle through a tiny re-export.
 */

/* Re-exported from autopilot.c -- the UART was opened during
 * autopilot_init() and the handle is owned over there. */
extern alp_uart_t *autopilot_rc_uart(void);   /* defined in autopilot.c */

void autopilot_rc_loop(autopilot_state_t *s)
{
    /* The autopilot owns the UART handle; we pull frame bytes via
     * a small re-export to keep this file pure-orchestration.
     *
     * last_frame_ms is our RC-link liveness watchdog -- if we go
     * 200 ms without a fresh frame, rc_link_ok flips false and the
     * nav loop's failsafe arbitrator promotes the airframe to
     * AP_MODE_FAILSAFE (controlled descent + land). */
    uint32_t last_frame_ms = 0;
    while (1) {
        sbus_frame_t f;
        /* TODO(sbus): real UART byte-stream framing -- find the
         * 0x0F start byte, then read 24 bytes.  Stubbed here so
         * the demo compiles standalone; v0.6 wires the real
         * frame slicer.  Pretend we got a frame every 20 ms so
         * the failsafe doesn't trip in the simulator.
         *
         * Real implementation will: alp_uart_read() into a small
         * byte ring, hunt for the 0x0F start byte, then accumulate
         * exactly 25 bytes and validate the 0x00 end byte. */
        if (true) {
            /* 1024 is the SBUS mid-stick value (range 0..2047,
             * mid = 1024 = "stick centred").  Filling every
             * channel with the neutral value keeps the airframe
             * level under sim. */
            for (int i = 0; i < SBUS_CHANNELS; i++) f.channel[i] = 1024;
            f.frame_lost = false;
            f.failsafe   = false;
            last_frame_ms = k_uptime_get_32();
        }

        /* Channel mapping (Futaba convention):
         *   ch1=roll  ch2=pitch  ch3=throttle  ch4=yaw  ch5=mode
         *
         * Roll/pitch/yaw are bidirectional sticks centred at 1024;
         * we normalise to [-1, +1] with ±800-count stick travel.
         * Throttle is unidirectional (bottom-of-detent = 0) so we
         * just divide by the 2048 full-scale. */
        s->stick_roll     = ((int)f.channel[0] - 1024) / 800.f;
        s->stick_pitch    = ((int)f.channel[1] - 1024) / 800.f;
        s->stick_throttle = (f.channel[2]) / 2048.f;
        s->stick_yaw      = ((int)f.channel[3] - 1024) / 800.f;

        /* Mode switch: ch5 below 600 -> MANUAL, 600..1400 -> STABILISE,
         * above 1400 -> GPS-HOLD.  Edge cases roll up to RTH via the
         * dedicated ch6 trip.
         *
         * The three-position switch on most RC transmitters maps to
         * roughly {300, 1024, 1700} in SBUS counts, so the 600/1400
         * thresholds give comfortable hysteresis around each detent. */
        if      (f.channel[4] < 600)  s->mode = AP_MODE_MANUAL;
        else if (f.channel[4] < 1400) s->mode = AP_MODE_STABILISE;
        else                          s->mode = AP_MODE_GPS_HOLD;
        /* ch6 is the dedicated RTH (return-to-home) trip -- a 2-pos
         * switch the pilot can hit to override whatever ch5 says. */
        if (f.channel[5] > 1400)      s->mode = AP_MODE_RTH;

        /* Arming -- ch7 high + throttle low.
         *
         * The throttle-low guard is non-negotiable: arming with
         * throttle up would spin motors immediately, which is how
         * fingers get cut on the bench.  ArduPilot and PX4 enforce
         * the same pre-arm check. */
        s->armed = (f.channel[6] > 1400) && (s->stick_throttle < 0.05f);
        /* If disarmed for any reason, force the mode back to
         * DISARMED so downstream loops can't accidentally spin
         * motors based on stale mode state. */
        if (!s->armed) s->mode = AP_MODE_DISARMED;

        /* RC link-OK if we've seen a frame in the last 200 ms.
         * 200 ms = 10 missed frames at 20 ms cadence, generous
         * enough to ride out transient radio dropouts without
         * triggering failsafe on every glitch. */
        s->rc_link_ok = (k_uptime_get_32() - last_frame_ms) < 200;

        /* 50 Hz nominal poll rate (20 ms).  SBUS frames arrive at
         * ~70 Hz natively, so we'll never miss two in a row at this
         * cadence even with worst-case jitter. */
        k_msleep(20);
    }
}
