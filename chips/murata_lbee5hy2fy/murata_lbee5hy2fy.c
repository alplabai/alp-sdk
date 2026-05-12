/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Murata LBEE5HY2FY-922 thin GPIO surface.  See <alp/chips/murata_lbee5hy2fy.h>
 * for the cross-side wiring rationale.
 */

#include <string.h>
#include <stdbool.h>

#include "alp/chips/murata_lbee5hy2fy.h"

alp_status_t murata_lbee5hy2fy_init(murata_lbee5hy2fy_t *ctx,
                                    murata_reg_set_t reg_set,
                                    murata_reg_get_t reg_get,
                                    void *reg_user,
                                    alp_gpio_t *bt_host_wake,
                                    alp_gpio_t *wl_host_wake,
                                    alp_gpio_t *bt_dev_wake)
{
    if (ctx == NULL || reg_set == NULL) return ALP_ERR_INVAL;
    memset(ctx, 0, sizeof(*ctx));
    ctx->reg_set       = reg_set;
    ctx->reg_get       = reg_get;
    ctx->reg_user      = reg_user;
    ctx->bt_host_wake  = bt_host_wake;
    ctx->wl_host_wake  = wl_host_wake;
    ctx->bt_dev_wake   = bt_dev_wake;

    /* Drive both REG_ON lines low so the module starts in the OFF
     * state regardless of POR-time residual levels. */
    int rv = reg_set(MURATA_REG_BT, false, reg_user);
    if (rv != 0) return ALP_ERR_IO;
    rv = reg_set(MURATA_REG_WL, false, reg_user);
    if (rv != 0) return ALP_ERR_IO;

    ctx->bt_powered  = false;
    ctx->wl_powered  = false;
    ctx->initialised = true;
    return ALP_OK;
}

alp_status_t murata_lbee5hy2fy_bt_power(murata_lbee5hy2fy_t *ctx, bool on)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    const int rv = ctx->reg_set(MURATA_REG_BT, on, ctx->reg_user);
    if (rv != 0) return ALP_ERR_IO;
    ctx->bt_powered = on;
    return ALP_OK;
}

alp_status_t murata_lbee5hy2fy_wl_power(murata_lbee5hy2fy_t *ctx, bool on)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    const int rv = ctx->reg_set(MURATA_REG_WL, on, ctx->reg_user);
    if (rv != 0) return ALP_ERR_IO;
    ctx->wl_powered = on;
    return ALP_OK;
}

alp_status_t murata_lbee5hy2fy_bt_wake_device(murata_lbee5hy2fy_t *ctx)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (ctx->bt_dev_wake == NULL) return ALP_ERR_NOSUPPORT;
    /* Active-low pulse per CYW55513 datasheet; the BT subsystem
     * exits sleep on the falling edge. */
    alp_status_t s = alp_gpio_write(ctx->bt_dev_wake, false);
    if (s != ALP_OK) return s;
    return alp_gpio_write(ctx->bt_dev_wake, true);
}

static alp_status_t read_input(alp_gpio_t *pin, bool *level)
{
    if (pin == NULL) {
        *level = false;
        return ALP_OK; /* Documented as "always-low" when line not wired. */
    }
    return alp_gpio_read(pin, level);
}

alp_status_t murata_lbee5hy2fy_bt_host_wake_level(murata_lbee5hy2fy_t *ctx, bool *level)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (level == NULL) return ALP_ERR_INVAL;
    return read_input(ctx->bt_host_wake, level);
}

alp_status_t murata_lbee5hy2fy_wl_host_wake_level(murata_lbee5hy2fy_t *ctx, bool *level)
{
    if (ctx == NULL || !ctx->initialised) return ALP_ERR_NOT_READY;
    if (level == NULL) return ALP_ERR_INVAL;
    return read_input(ctx->wl_host_wake, level);
}

void murata_lbee5hy2fy_deinit(murata_lbee5hy2fy_t *ctx)
{
    if (ctx == NULL) return;
    if (ctx->initialised && ctx->reg_set != NULL) {
        /* Best-effort power-down; ignore callback errors here -- the
         * caller may have already torn down the bus underneath us. */
        (void)ctx->reg_set(MURATA_REG_BT, false, ctx->reg_user);
        (void)ctx->reg_set(MURATA_REG_WL, false, ctx->reg_user);
    }
    ctx->initialised = false;
    ctx->reg_set     = NULL;
    ctx->reg_get     = NULL;
    ctx->reg_user    = NULL;
    ctx->bt_host_wake = NULL;
    ctx->wl_host_wake = NULL;
    ctx->bt_dev_wake  = NULL;
}
