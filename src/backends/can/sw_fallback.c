/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software CAN fallback.  Stateless stub for native_sim builds;
 * not a real bus.
 *
 * open()          -- succeeds; state is zero-initialised by the
 *                    dispatcher.  No frame ring is allocated -- the
 *                    stub never moves frames.
 * start()         -- no-op, returns ALP_OK.
 * stop()          -- no-op, returns ALP_OK.
 * send()          -- returns ALP_ERR_NOSUPPORT (no peer node to
 *                    receive the frame).
 * add_filter()    -- returns ALP_ERR_NOSUPPORT (no RX path to
 *                    dispatch through).
 * remove_filter() -- returns ALP_OK (idempotent on a stub that
 *                    never installed a filter to begin with).
 * close()         -- no-op.
 *
 * Faking CAN frame movement would tempt callers to validate a SW
 * loopback that the production backend does not provide; better to
 * surface NOSUPPORT loudly so test authors reach for the real
 * backend or hardware loopback (cfg->loopback=true).
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_CAN_SW_FALLBACK=y with no Zephyr
 * CAN devices present.
 *
 * @par Cost: ROM ~140 B, RAM 0 bytes (no per-handle state needed;
 *      the dispatcher's portable handle covers every observable).
 * @par Performance: O(1) per call; deterministic for test
 *      assertions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>

#include "can_ops.h"

static alp_status_t sw_open(const alp_can_config_t *cfg,
                            alp_can_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)cfg;
    st->dev     = NULL;
    st->bus_id  = 0u;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_start(alp_can_backend_state_t *st) {
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_stop(alp_can_backend_state_t *st) {
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_send(alp_can_backend_state_t *st,
                            const alp_can_frame_t *frame,
                            uint32_t timeout_ms) {
    (void)st; (void)frame; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_add_filter(alp_can_backend_state_t *st,
                                  const alp_can_filter_t *filter,
                                  alp_can_rx_cb_t cb,
                                  void *user,
                                  int32_t *filter_id_out) {
    (void)st; (void)filter; (void)cb; (void)user; (void)filter_id_out;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_remove_filter(alp_can_backend_state_t *st,
                                     int32_t filter_id) {
    (void)st; (void)filter_id;
    return ALP_OK;
}

static void sw_close(alp_can_backend_state_t *st) {
    (void)st;
}

static const alp_can_ops_t _ops = {
    .open          = sw_open,
    .start         = sw_start,
    .stop          = sw_stop,
    .send          = sw_send,
    .add_filter    = sw_add_filter,
    .remove_filter = sw_remove_filter,
    .close         = sw_close,
};

ALP_BACKEND_REGISTER(can, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
