/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software I2S fallback.  Stateless stub for native_sim builds;
 * not a real audio bus.
 *
 * open()  -- succeeds; state is zero-initialised by the dispatcher.
 *            No slab is allocated -- the stub never moves frames.
 * start() -- no-op, returns ALP_OK.
 * stop()  -- no-op, returns ALP_OK.
 * write() -- returns ALP_ERR_NOSUPPORT (no real DAC / codec to feed).
 * read()  -- returns ALP_ERR_NOSUPPORT (no real microphone source).
 * close() -- no-op.
 *
 * Faking I2S frame movement would tempt callers to validate a SW
 * loopback that the production backend does not provide; better to
 * surface NOSUPPORT loudly so test authors reach for the real
 * backend.
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_I2S_SW_FALLBACK=y with no Zephyr
 * I2S devices present.
 *
 * @par Cost: ROM ~120 B, RAM 0 bytes (no per-handle state needed;
 *      the dispatcher's portable handle covers every observable).
 * @par Performance: O(1) per call; deterministic for test
 *      assertions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i2s.h>

#include "i2s_ops.h"

static alp_status_t sw_open(const alp_i2s_config_t *cfg,
                            alp_i2s_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)cfg;
    st->dev     = NULL;
    st->bus_id  = 0u;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_start(alp_i2s_backend_state_t *st) {
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_stop(alp_i2s_backend_state_t *st) {
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_write(alp_i2s_backend_state_t *st,
                             const void *block, size_t bytes,
                             uint32_t timeout_ms) {
    (void)st; (void)block; (void)bytes; (void)timeout_ms;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_read(alp_i2s_backend_state_t *st,
                            void *block, size_t bytes,
                            size_t *bytes_out,
                            uint32_t timeout_ms) {
    (void)st; (void)block; (void)bytes; (void)timeout_ms;
    if (bytes_out != NULL) *bytes_out = 0u;
    return ALP_ERR_NOSUPPORT;
}

static void sw_close(alp_i2s_backend_state_t *st) {
    (void)st;
}

static const alp_i2s_ops_t _ops = {
    .open  = sw_open,
    .start = sw_start,
    .stop  = sw_stop,
    .write = sw_write,
    .read  = sw_read,
    .close = sw_close,
};

ALP_BACKEND_REGISTER(i2s, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
