/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software USB fallback.  Wildcard backend at priority 0 -- picked
 * only when no hardware backend is linked into the build
 * (native_sim trimmed-image case).  No real USB controller exists
 * under native_sim, so the stub lets examples that include
 * <alp/usb.h> compile and exercise the dispatcher.
 *
 * Open succeeds (device + host both); all I/O ops return
 * ALP_ERR_NOT_IMPLEMENTED -- matches the design spec Section 5
 * sw_fallback contract.
 *
 * @par Cost: ROM ~400 B, zero RAM (no per-handle state).
 * @par Performance: O(1) per call; every I/O op short-circuits to
 *      ALP_ERR_NOT_IMPLEMENTED with no Zephyr-subsystem touch.
 */

#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/usb.h>

#include "usb_ops.h"

/* ---------- Device-side ops ---------- */

static alp_status_t sw_dev_open(const alp_usb_device_config_t *cfg,
                                alp_usb_dev_state_t *st,
                                alp_capabilities_t *caps_out)
{
    (void)cfg;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_dev_enable(alp_usb_dev_state_t *st)
{
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_dev_disable(alp_usb_dev_state_t *st)
{
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_dev_write(alp_usb_dev_state_t *st,
                                 const uint8_t *data, size_t len,
                                 uint32_t timeout_ms)
{
    (void)st;
    (void)data;
    (void)len;
    (void)timeout_ms;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static alp_status_t sw_dev_read(alp_usb_dev_state_t *st,
                                uint8_t *data, size_t len,
                                size_t *out_len, uint32_t timeout_ms)
{
    (void)st;
    (void)data;
    (void)len;
    (void)timeout_ms;
    if (out_len != NULL) *out_len = 0;
    return ALP_ERR_NOT_IMPLEMENTED;
}

static void sw_dev_close(alp_usb_dev_state_t *st)
{
    (void)st;
}

/* ---------- Host-side ops ---------- */

static alp_status_t sw_host_open(alp_usb_host_state_t *st,
                                 alp_capabilities_t *caps_out)
{
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_host_enable(alp_usb_host_state_t *st)
{
    (void)st;
    return ALP_OK;
}

static alp_status_t sw_host_disable(alp_usb_host_state_t *st)
{
    (void)st;
    return ALP_OK;
}

static void sw_host_close(alp_usb_host_state_t *st)
{
    (void)st;
}

/* ---------- Registration ---------- */

static const alp_usb_ops_t _ops = {
    .dev_open    = sw_dev_open,
    .dev_enable  = sw_dev_enable,
    .dev_disable = sw_dev_disable,
    .dev_write   = sw_dev_write,
    .dev_read    = sw_dev_read,
    .dev_close   = sw_dev_close,

    .host_open    = sw_host_open,
    .host_enable  = sw_host_enable,
    .host_disable = sw_host_disable,
    .host_close   = sw_host_close,
};

ALP_BACKEND_REGISTER(usb, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
