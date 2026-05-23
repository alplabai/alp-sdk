/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_uart dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_UART_OPS_H
#define ALP_BACKENDS_UART_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_uart_ops alp_uart_ops_t;

typedef struct alp_uart_backend_state {
    const struct device     *dev;        /* Zephyr backend device pointer */
    uint32_t                 port_id;
    void                    *be_data;
    const alp_uart_ops_t    *ops;
} alp_uart_backend_state_t;

struct alp_uart_ops {
    alp_status_t (*open)(const alp_uart_config_t *cfg,
                         alp_uart_backend_state_t *state,
                         alp_capabilities_t *caps_out);
    alp_status_t (*write)(alp_uart_backend_state_t *state,
                          const uint8_t *data, size_t len);
    alp_status_t (*read)(alp_uart_backend_state_t *state,
                         uint8_t *data, size_t len,
                         uint32_t timeout_ms);
    void         (*close)(alp_uart_backend_state_t *state);
};

struct alp_uart {
    alp_uart_backend_state_t  state;
    const alp_backend_t      *backend;
    alp_capabilities_t        cached_caps;
    bool                      in_use;
};

#endif /* ALP_BACKENDS_UART_OPS_H */
