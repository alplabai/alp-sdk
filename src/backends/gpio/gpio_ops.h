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

/* Platform-backend open that takes the owning portable handle EXPLICITLY.
 *
 * The Zephyr backend has to know which struct alp_gpio an interrupt belongs to,
 * because the dispatcher stashes the user callback on the handle (pin->cb) and
 * the ISR thunk invokes it from there.  Its own ops->open() recovers that owner
 * with CONTAINER_OF(st, struct alp_gpio, state), which is correct only when the
 * caller really did pass &handle->state.
 *
 * A delegating backend does not: the CC3501E proxy owns the handle's state and
 * hands the platform backend a state object nested in its own per-handle
 * sidecar.  CONTAINER_OF on that yields a pointer some bytes BEFORE a
 * proxy_side_t, and the ISR thunk then loads a callback from whatever lies
 * there and calls it -- from interrupt context (issue #1618).
 *
 * So a delegating caller must name the owner instead of having it inferred.
 * Pass the OWNER'S state (&handle->state), not the handle: that keeps the
 * CONTAINER_OF -- and every Zephyr header it needs -- inside the platform
 * backend, so a delegating backend stays free of Zephyr types.  NULL is
 * accepted and leaves the pin without a callback target, which is safer than
 * fabricating one. */
alp_status_t alp_z_gpio_open_owned(uint32_t                  pin_id,
                                   alp_gpio_backend_state_t *st,
                                   alp_capabilities_t       *caps_out,
                                   alp_gpio_backend_state_t *owner_state);

#endif /* ALP_BACKENDS_GPIO_OPS_H */
