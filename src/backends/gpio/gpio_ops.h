/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_gpio dispatcher and per-backend
 * implementations.  NOT a public header.
 */

#ifndef ALP_BACKENDS_GPIO_OPS_H
#define ALP_BACKENDS_GPIO_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_gpio_ops alp_gpio_ops_t;

typedef struct alp_gpio_backend_state {
	void                 *dev; /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
	uint32_t              pin_id;
	void                 *be_data;
	const alp_gpio_ops_t *ops;
} alp_gpio_backend_state_t;

struct alp_gpio_ops {
	alp_status_t (*open)(uint32_t                  pin_id,
	                     alp_gpio_backend_state_t *state,
	                     alp_capabilities_t       *caps_out);
	alp_status_t (*configure)(alp_gpio_backend_state_t *state,
	                          alp_gpio_dir_t            dir,
	                          alp_gpio_pull_t           pull);
	alp_status_t (*write)(alp_gpio_backend_state_t *state, bool level);
	alp_status_t (*read)(alp_gpio_backend_state_t *state, bool *level);
	alp_status_t (*enable_irq)(alp_gpio_backend_state_t *state,
	                           alp_gpio_edge_t           edge,
	                           alp_gpio_cb_t             cb,
	                           void                     *user);
	alp_status_t (*disable_irq)(alp_gpio_backend_state_t *state);
	void (*close)(alp_gpio_backend_state_t *state);
};

/*
 * Portable handle layout.  The user-facing edge / cb / cb_user fields
 * live here so non-Zephyr backends can drive them without dragging in
 * <zephyr/drivers/gpio.h>.  Zephyr-specific glue (struct gpio_callback)
 * lives in a sidecar inside src/backends/gpio/zephyr_drv.c.
 */
struct alp_gpio {
	alp_gpio_backend_state_t state;
	const alp_backend_t     *backend;
	alp_capabilities_t       cached_caps;
	alp_gpio_dir_t           dir;
	alp_gpio_pull_t          pull;
	alp_gpio_edge_t          edge;
	alp_gpio_cb_t            cb;
	void                    *cb_user;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed (with in_use) after the
	 * fields alp_gpio_open() explicitly re-initialises post-claim, so
	 * moving in_use to the last member (required for the atomic-claim
	 * zeroing in src/gpio_dispatch.c: memset up to offsetof(...,
	 * in_use)) doesn't change what a fresh claim zeroes. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

/* Platform (Zephyr) gpio backend ops accessor -- defined in zephyr_drv.c.
 * The CC3501E GPIO proxy backend (cc3501e_proxy.c) delegates its non-bridge
 * pins here so it reuses the real Zephyr pin I/O instead of re-implementing
 * it.  NULL is never returned. */
const alp_gpio_ops_t *alp_z_gpio_ops(void);

#endif /* ALP_BACKENDS_GPIO_OPS_H */
