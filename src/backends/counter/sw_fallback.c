/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software counter fallback.  Deterministic tick generator for
 * native_sim builds; not a real timer.  Alarms are NOT supported
 * in the SW backend -- a true callback would need a host-side
 * timer thread which the test surface doesn't justify.
 *
 * @par Cost: ROM ~400 B, RAM 8 B (running tick counter + started flag).
 * @par Performance: O(1) per call; get_value advances the cursor by
 *      one each call.  Tests assert deterministic values.
 */

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "counter_ops.h"

static uint32_t _ticks = 0u;
static bool     _running = false;

static alp_status_t sw_open(const alp_counter_config_t *cfg,
                            alp_counter_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)cfg;
    st->dev = NULL;
    st->counter_id = cfg->counter_id;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_start(alp_counter_backend_state_t *st) {
    (void)st; _running = true; return ALP_OK;
}

static alp_status_t sw_stop(alp_counter_backend_state_t *st) {
    (void)st; _running = false; return ALP_OK;
}

static alp_status_t sw_get_value(alp_counter_backend_state_t *st, uint32_t *ticks_out) {
    (void)st;
    *ticks_out = _ticks;
    if (_running) _ticks += 1u;
    return ALP_OK;
}

static alp_status_t sw_us_to_ticks(alp_counter_backend_state_t *st, uint32_t us, uint32_t *ticks_out) {
    (void)st;
    *ticks_out = us;     /* 1 us == 1 tick on the SW backend */
    return ALP_OK;
}

static alp_status_t sw_set_alarm(alp_counter_backend_state_t *st,
                                 uint32_t ticks_from_now,
                                 struct alp_counter *owner) {
    (void)st; (void)ticks_from_now; (void)owner;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_cancel_alarm(alp_counter_backend_state_t *st) {
    (void)st; return ALP_OK;
}

static const alp_counter_ops_t _ops = {
    .open         = sw_open,
    .start        = sw_start,
    .stop         = sw_stop,
    .get_value    = sw_get_value,
    .us_to_ticks  = sw_us_to_ticks,
    .set_alarm    = sw_set_alarm,
    .cancel_alarm = sw_cancel_alarm,
    .close        = NULL,
};

ALP_BACKEND_REGISTER(counter, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
