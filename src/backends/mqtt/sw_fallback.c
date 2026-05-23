/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software MQTT fallback.  Wildcard backend at priority 0 -- picked
 * only when no hardware backend is linked into the build (native_sim
 * trimmed-image case).  No real broker is reachable under native_sim,
 * so the stub lets examples that include <alp/iot.h> compile and
 * exercise the dispatcher without pulling in CONFIG_MQTT_LIB +
 * CONFIG_NET_TCP.
 *
 * Contract:
 *   - open()       -> ALP_OK (no broker setup)
 *   - connect()    -> ALP_ERR_NOT_IMPLEMENTED (no real broker)
 *   - publish()    -> ALP_ERR_NOT_IMPLEMENTED
 *   - subscribe()  -> ALP_ERR_NOT_IMPLEMENTED
 *   - loop()       -> ALP_ERR_NOT_IMPLEMENTED
 *   - close()      -> no-op
 *
 * Matches the design spec Section 5 sw_fallback contract.
 *
 * @par Cost: ROM ~140 B, zero RAM (no per-handle state).
 * @par Performance: O(1) per call; every op short-circuits to
 *      ALP_OK / ALP_ERR_NOT_IMPLEMENTED with no Zephyr-subsystem
 *      touch.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

#include "mqtt_ops.h"

static alp_status_t sw_open(const alp_mqtt_config_t *cfg,
                            alp_mqtt_backend_state_t *st,
                            alp_capabilities_t *caps_out)
{
    (void)cfg;
    st->be_data     = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_connect(alp_mqtt_backend_state_t *st, uint32_t timeout_ms)
{
    (void)st;
    (void)timeout_ms;
    /* No real broker under native_sim, so returning NOT_IMPLEMENTED
     * (rather than OK) keeps callers from thinking they've connected
     * when they haven't. */
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_publish(alp_mqtt_backend_state_t *st, const char *topic,
                               const uint8_t *payload, size_t len,
                               alp_mqtt_qos_t qos, bool retain)
{
    (void)st;
    (void)topic;
    (void)payload;
    (void)len;
    (void)qos;
    (void)retain;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_subscribe(alp_mqtt_backend_state_t *st, const char *topic_filter,
                                 alp_mqtt_qos_t qos, alp_mqtt_msg_cb_t cb, void *user)
{
    (void)st;
    (void)topic_filter;
    (void)qos;
    (void)cb;
    (void)user;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_loop(alp_mqtt_backend_state_t *st, uint32_t timeout_ms)
{
    (void)st;
    (void)timeout_ms;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_close(alp_mqtt_backend_state_t *st)
{
    (void)st;
}

static const alp_mqtt_ops_t _ops = {
    .open      = sw_open,
    .connect   = sw_connect,
    .publish   = sw_publish,
    .subscribe = sw_subscribe,
    .loop      = sw_loop,
    .close     = sw_close,
};

ALP_BACKEND_REGISTER(mqtt, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
