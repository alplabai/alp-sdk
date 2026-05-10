/*
 * Copyright 2026 ALP Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Zephyr backend for <alp/peripheral.h> — UART (poll-based v0.1).
 */

#include <errno.h>

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/util.h>

#include "alp/peripheral.h"
#include "alp/soc_caps.h"
#include "handles.h"

#define ALP_UART_DEV_OR_NULL(idx)                                              \
    COND_CODE_1(DT_NODE_EXISTS(DT_ALIAS(_CONCAT(alp_uart, idx))),              \
                (DEVICE_DT_GET(DT_ALIAS(_CONCAT(alp_uart, idx)))), (NULL))

static const struct device *const alp_uart_devs[] = {
    ALP_UART_DEV_OR_NULL(0),
    ALP_UART_DEV_OR_NULL(1),
    ALP_UART_DEV_OR_NULL(2),
    ALP_UART_DEV_OR_NULL(3),
    ALP_UART_DEV_OR_NULL(4),
    ALP_UART_DEV_OR_NULL(5),
    ALP_UART_DEV_OR_NULL(6),
    ALP_UART_DEV_OR_NULL(7),
};

static enum uart_config_parity to_zephyr_parity(alp_uart_parity_t p) {
    switch (p) {
    case ALP_UART_PARITY_EVEN: return UART_CFG_PARITY_EVEN;
    case ALP_UART_PARITY_ODD:  return UART_CFG_PARITY_ODD;
    default:                   return UART_CFG_PARITY_NONE;
    }
}

static enum uart_config_data_bits to_zephyr_data_bits(uint8_t bits) {
    switch (bits) {
    case 5:  return UART_CFG_DATA_BITS_5;
    case 6:  return UART_CFG_DATA_BITS_6;
    case 7:  return UART_CFG_DATA_BITS_7;
    case 9:  return UART_CFG_DATA_BITS_9;
    case 8:
    default: return UART_CFG_DATA_BITS_8;
    }
}

static enum uart_config_stop_bits to_zephyr_stop_bits(uint8_t bits) {
    return (bits == 2) ? UART_CFG_STOP_BITS_2 : UART_CFG_STOP_BITS_1;
}

static alp_status_t errno_to_alp(int err) {
    switch (err) {
    case 0:        return ALP_OK;
    case -EINVAL:  return ALP_ERR_INVAL;
    case -EBUSY:   return ALP_ERR_BUSY;
    case -ETIMEDOUT: return ALP_ERR_TIMEOUT;
    case -EIO:     return ALP_ERR_IO;
    case -ENOTSUP:
    case -ENOSYS:  return ALP_ERR_NOSUPPORT;
    default:       return ALP_ERR_IO;
    }
}

alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg) {
    alp_z_clear_last_error();

    if (cfg == NULL) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->port_id >= ARRAY_SIZE(alp_uart_devs)) {
        alp_z_set_last_error(ALP_ERR_INVAL);
        return NULL;
    }
    if (cfg->port_id >= ALP_SOC_UART_COUNT) {
        alp_z_set_last_error(ALP_ERR_OUT_OF_RANGE);
        return NULL;
    }

    const struct device *dev = alp_uart_devs[cfg->port_id];
    if (dev == NULL || !device_is_ready(dev)) {
        alp_z_set_last_error(ALP_ERR_NOT_READY);
        return NULL;
    }

    struct uart_config zcfg = {
        .baudrate  = cfg->baudrate,
        .parity    = to_zephyr_parity(cfg->parity),
        .stop_bits = to_zephyr_stop_bits(cfg->stop_bits),
        .data_bits = to_zephyr_data_bits(cfg->data_bits),
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int err = uart_configure(dev, &zcfg);
    /* Some controllers / shims don't expose runtime configuration —
     * accept ENOSYS / ENOTSUP and trust the devicetree-provided params. */
    if (err != 0 && err != -ENOSYS && err != -ENOTSUP) {
        alp_z_set_last_error(errno_to_alp(err));
        return NULL;
    }

    struct alp_uart *h = alp_z_uart_pool_acquire();
    if (h == NULL) {
        alp_z_set_last_error(ALP_ERR_NOMEM);
        return NULL;
    }
    h->port_id = cfg->port_id;
    h->dev     = dev;
    h->cfg     = *cfg;
    return h;
}

void alp_uart_close(alp_uart_t *port) {
    alp_z_uart_pool_release(port);
}

alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len) {
    if (port == NULL || !port->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;
    for (size_t i = 0; i < len; i++) {
        uart_poll_out(port->dev, data[i]);
    }
    return ALP_OK;
}

alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len,
                           uint32_t timeout_ms) {
    if (port == NULL || !port->in_use) return ALP_ERR_NOT_READY;
    if (data == NULL && len > 0) return ALP_ERR_INVAL;

    const int64_t deadline = (timeout_ms == 0)
        ? INT64_MAX
        : k_uptime_get() + (int64_t)timeout_ms;

    for (size_t i = 0; i < len; i++) {
        int err;
        do {
            err = uart_poll_in(port->dev, &data[i]);
            if (err == -1 && k_uptime_get() >= deadline) {
                return ALP_ERR_TIMEOUT;
            }
            if (err == -1) {
                /* k_msleep(1) instead of k_yield() so the system
                 * tick actually advances on native_sim (k_yield
                 * with no other ready thread is a no-op there,
                 * making the timeout deadline unreachable). */
                k_msleep(1);
            }
        } while (err == -1);

        if (err != 0) return errno_to_alp(err);
    }
    return ALP_OK;
}
