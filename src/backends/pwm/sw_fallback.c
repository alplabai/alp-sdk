/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software PWM fallback.  Stateless stub for native_sim builds;
 * not a real channel.
 *
 * open()          -- succeeds; primes the portable handle's
 *                    period_ns / flags from the config so subsequent
 *                    dispatcher bounds-checks against pulse_ns work
 *                    as if the channel were real.
 * set_duty()      -- no-op, returns ALP_OK (pulse_ns discarded).
 * set_period()    -- no-op, returns ALP_OK; the dispatcher updates
 *                    its cached period_ns regardless.
 * configure()     -- returns ALP_ERR_NOSUPPORT (no underlying timer
 *                    to honour dead-time / alignment).
 * single_pulse()  -- returns ALP_ERR_NOSUPPORT (no one-shot timer
 *                    primitive to fake).
 * capture_open()  -- returns ALP_ERR_NOSUPPORT (no edge source).
 * capture_read()  -- returns ALP_ERR_NOSUPPORT.
 * capture_close() -- no-op.
 * close()         -- no-op.
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_PWM_SW_FALLBACK=y with no Zephyr
 * PWM devices present.
 *
 * @par Cost: ROM ~150 B, RAM 0 bytes (no per-handle state needed;
 *      the dispatcher's portable handle covers every observable).
 * @par Performance: O(1) per call; deterministic for test
 *      assertions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* CONTAINER_OF without <zephyr/sys/util.h>: this TU is also linked into the
 * Yocto/host build (src/yocto/CMakeLists.txt), whose toolchain has no Zephyr
 * headers.  offsetof comes from <stddef.h> above.  Guarded so a Zephyr build
 * that already defines it (transitively) keeps its own. */
#ifndef CONTAINER_OF
#define CONTAINER_OF(ptr, type, member) ((type *)((char *)(ptr)-offsetof(type, member)))
#endif

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/pwm.h>

#include "pwm_ops.h"

static alp_status_t sw_open(const alp_pwm_config_t *cfg,
                            alp_pwm_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    struct alp_pwm *h = CONTAINER_OF(st, struct alp_pwm, state);
    h->channel   = cfg->channel_id;
    h->period_ns = (cfg->period_ns != 0u) ? cfg->period_ns : 1000000u; /* 1 kHz */
    h->flags     = 0u;

    st->dev        = NULL;
    st->channel_id = cfg->channel_id;
    st->be_data    = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_set_duty(alp_pwm_backend_state_t *st, uint32_t pulse_ns) {
    (void)st; (void)pulse_ns;
    return ALP_OK;
}

static alp_status_t sw_set_period(alp_pwm_backend_state_t *st, uint32_t period_ns) {
    (void)st; (void)period_ns;
    return ALP_OK;
}

static alp_status_t sw_configure(alp_pwm_backend_state_t *st,
                                 alp_pwm_align_t align_mode,
                                 uint32_t dead_time_ns,
                                 uint8_t break_cfg) {
    (void)st; (void)align_mode; (void)dead_time_ns; (void)break_cfg;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_single_pulse(alp_pwm_backend_state_t *st,
                                    uint32_t pulse_ns) {
    (void)st; (void)pulse_ns;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_capture_open(const alp_pwm_capture_config_t *cfg,
                                    alp_pwm_backend_state_t *st,
                                    alp_capabilities_t *caps_out) {
    (void)cfg; (void)st; (void)caps_out;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_capture_read(alp_pwm_backend_state_t *st,
                                    uint32_t *period_ns_out,
                                    uint32_t *pulse_ns_out) {
    (void)st;
    if (period_ns_out != NULL) *period_ns_out = 0u;
    if (pulse_ns_out  != NULL) *pulse_ns_out  = 0u;
    return ALP_ERR_NOSUPPORT;
}

static void sw_capture_close(alp_pwm_backend_state_t *st) { (void)st; }
static void sw_close(alp_pwm_backend_state_t *st) { (void)st; }

static const alp_pwm_ops_t _ops = {
    .open          = sw_open,
    .set_duty      = sw_set_duty,
    .set_period    = sw_set_period,
    .configure     = sw_configure,
    .single_pulse  = sw_single_pulse,
    .capture_open  = sw_capture_open,
    .capture_read  = sw_capture_read,
    .capture_close = sw_capture_close,
    .close         = sw_close,
};

ALP_BACKEND_REGISTER(pwm, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
