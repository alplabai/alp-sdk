/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_counter dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_COUNTER_OPS_H
#define ALP_BACKENDS_COUNTER_OPS_H

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/device.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

typedef struct alp_counter_ops alp_counter_ops_t;

typedef struct alp_counter_backend_state {
    const struct device     *dev;            /* Zephyr backend; NULL for bridge */
    uint32_t                 counter_id;
    alp_counter_alarm_cb_t   alarm_cb;       /* user callback */
    void                    *alarm_user;
    void                    *be_data;
    const alp_counter_ops_t *ops;
} alp_counter_backend_state_t;

struct alp_counter_ops {
    alp_status_t (*open)(const alp_counter_config_t *cfg,
                         alp_counter_backend_state_t *state,
                         alp_capabilities_t *caps_out);
    alp_status_t (*start)(alp_counter_backend_state_t *state);
    alp_status_t (*stop)(alp_counter_backend_state_t *state);
    alp_status_t (*get_value)(alp_counter_backend_state_t *state, uint32_t *ticks_out);
    alp_status_t (*us_to_ticks)(alp_counter_backend_state_t *state, uint32_t us, uint32_t *ticks_out);
    alp_status_t (*set_alarm)(alp_counter_backend_state_t *state,
                              uint32_t ticks_from_now,
                              struct alp_counter *owner /* for trampoline back-ref */);
    alp_status_t (*cancel_alarm)(alp_counter_backend_state_t *state);
    void         (*close)(alp_counter_backend_state_t *state);
};

struct alp_counter {
    alp_counter_backend_state_t  state;
    const alp_backend_t         *backend;
    alp_capabilities_t           cached_caps;
    bool                         in_use;
};

#endif /* ALP_BACKENDS_COUNTER_OPS_H */
