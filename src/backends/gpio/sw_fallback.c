/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software GPIO fallback.  Stateless stub for native_sim builds;
 * not a real pin.
 *
 * open()       -- succeeds; state is zero-initialised by the dispatcher.
 * configure()  -- no-op, returns ALP_OK.
 * write()      -- no-op, returns ALP_OK (level discarded).
 * read()       -- writes false into *level and returns ALP_OK.
 * irq_enable() -- returns ALP_ERR_NOSUPPORT (no edge source to hook).
 * irq_disable()-- returns ALP_OK (paired no-op for the dispatcher).
 * close()      -- no-op.
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_GPIO_SW_FALLBACK=y with no Zephyr
 * GPIO devices present.
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
#include <alp/peripheral.h>

#include "gpio_ops.h"

static alp_status_t sw_open(uint32_t pin_id,
                            alp_gpio_backend_state_t *st,
                            alp_capabilities_t *caps_out) {
    (void)pin_id;
    st->dev     = NULL;
    st->pin_id  = pin_id;
    st->be_data = NULL;
    caps_out->flags = 0u;
    return ALP_OK;
}

static alp_status_t sw_configure(alp_gpio_backend_state_t *st,
                                 alp_gpio_dir_t dir,
                                 alp_gpio_pull_t pull) {
    (void)st; (void)dir; (void)pull;
    return ALP_OK;
}

static alp_status_t sw_write(alp_gpio_backend_state_t *st, bool level) {
    (void)st; (void)level;
    return ALP_OK;
}

static alp_status_t sw_read(alp_gpio_backend_state_t *st, bool *level) {
    (void)st;
    if (level != NULL) *level = false;
    return ALP_OK;
}

static alp_status_t sw_irq_enable(alp_gpio_backend_state_t *st,
                                  alp_gpio_edge_t edge,
                                  alp_gpio_cb_t cb,
                                  void *user) {
    (void)st; (void)edge; (void)cb; (void)user;
    return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_irq_disable(alp_gpio_backend_state_t *st) {
    (void)st;
    return ALP_OK;
}

static void sw_close(alp_gpio_backend_state_t *st) {
    (void)st;
}

static const alp_gpio_ops_t _ops = {
    .open        = sw_open,
    .configure   = sw_configure,
    .write       = sw_write,
    .read        = sw_read,
    .enable_irq  = sw_irq_enable,
    .disable_irq = sw_irq_disable,
    .close       = sw_close,
};

ALP_BACKEND_REGISTER(gpio, sw_fallback, {
    .silicon_ref = "*",
    .vendor      = "sw_fallback",
    .base_caps   = 0u,
    .priority    = 0,
    .ops         = &_ops,
    .probe       = NULL,
});
