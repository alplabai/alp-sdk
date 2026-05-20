/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * drone-hud
 * =========
 *
 * Marketing-grade demo: an E1M-AEN module renders a full drone
 * telemetry HUD on a 240x320 TFT, sourcing live data from:
 *   - LSM6DSO 6-axis IMU (accel + gyro).
 *   - uBlox NEO-M9N GNSS receiver.
 *   - INA236 battery voltage + current monitor.
 *
 * Showcases the v0.5 SDK story end-to-end: one app drives four
 * different chips through the portable `<alp/...>` peripheral
 * surfaces, the `madgwick_ahrs` library fuses raw IMU samples
 * into a stable quaternion, and LVGL composes the result into
 * an artificial-horizon HUD that wouldn't look out of place on
 * a commercial flight controller.
 *
 *
 * ── Thread architecture ────────────────────────────────────────
 *
 *   main thread          -- LVGL render loop (30 fps).
 *   imu sample thread    -- LSM6DSO read + Madgwick fuse @ 100 Hz.
 *   slow telem thread    -- GNSS NMEA parse + INA236 read @ 5 Hz.
 *
 * The slow-telemetry thread sleeps in 200 ms slices, so it never
 * keeps the LVGL renderer waiting.  The IMU thread runs at a
 * higher priority than main so attitude updates land on time.
 *
 * Shared state is the `drone_telemetry_t` struct in sensors.h --
 * single-writer + single-reader per field, so a plain volatile
 * snapshot read from main is correct without locks.
 *
 *
 * ── What "verified" would mean ─────────────────────────────────
 *
 * Today the chips are paper-correct; lock-in arrives when:
 *  1. The IMU/GPS/battery readings make sense (units + scale).
 *  2. The attitude estimate doesn't drift when the SoM is held
 *     still (Madgwick beta tuning).
 *  3. The frame-rate stays above 25 fps with the full HUD live.
 *  4. The battery-remaining estimator survives a real
 *     charge/discharge cycle without lying.
 * That's the v0.6 AEN HiL story.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/drivers/display.h>

#include <lvgl.h>

#include "sensors.h"
#include "hud_ui.h"

LOG_MODULE_REGISTER(drone_hud, LOG_LEVEL_INF);

/* Shared telemetry snapshot, populated by the sensor threads and
 * read by the LVGL render loop in main(). */
static drone_telemetry_t g_telem;

/* IMU thread: ~100 Hz LSM6DSO sample + Madgwick attitude fusion.
 * Higher priority than main so a blocking LVGL blit doesn't shift
 * the IMU's sample timing. */
K_THREAD_STACK_DEFINE(imu_stack, 4096);
static struct k_thread imu_thread;

static void imu_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    drone_sensors_run_imu_loop(&g_telem);
}

/* Slow-telemetry thread: 5 Hz GPS NMEA + INA236 read.  Lower
 * priority than main; GPS reads happen in the background while
 * LVGL renders. */
K_THREAD_STACK_DEFINE(telem_stack, 4096);
static struct k_thread telem_thread;

static void telem_entry(void *p1, void *p2, void *p3)
{
    ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
    drone_sensors_run_slow_loop(&g_telem);
}

int main(void)
{
    LOG_INF("drone-hud demo starting");

    /* Sensor bring-up.  Returns immediately if a chip is absent so
     * the demo still runs on a partial board (e.g. no GPS lock
     * indoors -- the IMU + battery + UI still work). */
    if (drone_sensors_init(&g_telem) != 0) {
        LOG_WRN("drone_sensors_init returned non-zero; degraded mode");
    }

    /* Display + LVGL bring-up. */
    const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
    if (!device_is_ready(display)) {
        LOG_ERR("display %s not ready", display->name);
        return 1;
    }
    lv_init();
    hud_ui_build();             /* Construct the HUD layout once. */
    display_blanking_off(display);

    /* Spawn the sensor threads after the UI is built so the first
     * frame doesn't show garbage values. */
    k_thread_create(&imu_thread, imu_stack, K_THREAD_STACK_SIZEOF(imu_stack),
                    imu_entry, NULL, NULL, NULL,
                    /* priority */ K_PRIO_PREEMPT(2),
                    /* options  */ 0,
                    K_NO_WAIT);
    k_thread_name_set(&imu_thread, "imu_loop");

    k_thread_create(&telem_thread, telem_stack, K_THREAD_STACK_SIZEOF(telem_stack),
                    telem_entry, NULL, NULL, NULL,
                    /* priority */ K_PRIO_PREEMPT(6),
                    /* options  */ 0,
                    K_NO_WAIT);
    k_thread_name_set(&telem_thread, "slow_telem");

    /* Main loop: drain the LVGL task queue + refresh the on-screen
     * widgets from the shared telemetry snapshot.  We snapshot
     * the struct once per frame so a partial sensor update mid-
     * render doesn't show a torn value. */
    while (1) {
        drone_telemetry_t snapshot = g_telem;
        hud_ui_apply_telemetry(&snapshot);

        const uint32_t sleep_ms = lv_task_handler();
        k_msleep(MIN(sleep_ms, 10u));
    }

    return 0; /* unreachable. */
}
