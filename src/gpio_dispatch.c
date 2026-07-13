/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPIO class dispatcher.  Routes the public alp_gpio_* API
 * through the .alp_backends_gpio registry.
 *
 * The user-level IRQ callback (alp_gpio_cb_t) is stashed in the
 * portable handle so non-Zephyr backends can drive it without
 * dragging in <zephyr/drivers/gpio.h>; the Zephyr-side gpio_callback
 * sidecar lives in src/backends/gpio/zephyr_drv.c.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/gpio/gpio_ops.h"

ALP_BACKEND_DEFINE_CLASS(gpio);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_GPIO_HANDLES
#define CONFIG_ALP_SDK_MAX_GPIO_HANDLES 16
#endif

static struct alp_gpio _pool[CONFIG_ALP_SDK_MAX_GPIO_HANDLES];

static struct alp_gpio *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_GPIO_HANDLES; ++i) {
		/* Atomic claim (issue #629): only the winner of the flag flip
		 * may touch the slot's other fields -- in_use is the
		 * struct's last member, so zero everything before it. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_gpio, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_gpio *h)
{
	alp_slot_release(&h->in_use);
}

alp_gpio_t *alp_gpio_open(uint32_t pin_id)
{
	alp_z_clear_last_error();
	const alp_backend_t *be = alp_backend_select("gpio", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_gpio_ops_t *ops = (const alp_gpio_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_gpio *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(pin_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->open(pin_id, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	h->dir         = ALP_GPIO_INPUT;
	h->pull        = ALP_GPIO_PULL_NONE;
	h->edge        = ALP_GPIO_EDGE_NONE;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

/* Every op below gates on the lifecycle byte via alp_handle_op_enter(),
 * not in_use -- in_use is now touched only by the atomic claim/release
 * in _alloc/_free (issue #629: mixing an atomic in_use with a plain
 * read elsewhere is still a data race).  A racing alp_gpio_close()
 * cannot free the slot until every entered op has left. */

alp_status_t alp_gpio_configure(alp_gpio_t *pin, alp_gpio_dir_t dir, alp_gpio_pull_t pull)
{
	if (pin == NULL || !alp_handle_op_enter(&pin->lifecycle, &pin->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (pin->state.ops->configure == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = pin->state.ops->configure(&pin->state, dir, pull);
		if (rc == ALP_OK) {
			pin->dir  = dir;
			pin->pull = pull;
		}
	}
	alp_handle_op_leave(&pin->active_ops);
	return rc;
}

alp_status_t alp_gpio_write(alp_gpio_t *pin, bool level)
{
	if (pin == NULL || !alp_handle_op_enter(&pin->lifecycle, &pin->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc = (pin->state.ops->write == NULL) ? ALP_ERR_NOSUPPORT
	                                                  : pin->state.ops->write(&pin->state, level);
	alp_handle_op_leave(&pin->active_ops);
	return rc;
}

alp_status_t alp_gpio_read(alp_gpio_t *pin, bool *level)
{
	if (pin == NULL || !alp_handle_op_enter(&pin->lifecycle, &pin->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (level == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (pin->state.ops->read == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc = pin->state.ops->read(&pin->state, level);
	}
	alp_handle_op_leave(&pin->active_ops);
	return rc;
}

alp_status_t
alp_gpio_irq_enable(alp_gpio_t *pin, alp_gpio_edge_t edge, alp_gpio_cb_t cb, void *user)
{
	if (pin == NULL || !alp_handle_op_enter(&pin->lifecycle, &pin->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (edge == ALP_GPIO_EDGE_NONE || cb == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (pin->state.ops->enable_irq == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		pin->cb      = cb;
		pin->cb_user = user;
		pin->edge    = edge;
		rc           = pin->state.ops->enable_irq(&pin->state, edge, cb, user);
		if (rc != ALP_OK) {
			pin->cb      = NULL;
			pin->cb_user = NULL;
			pin->edge    = ALP_GPIO_EDGE_NONE;
		}
	}
	alp_handle_op_leave(&pin->active_ops);
	return rc;
}

alp_status_t alp_gpio_irq_disable(alp_gpio_t *pin)
{
	if (pin == NULL || !alp_handle_op_enter(&pin->lifecycle, &pin->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (pin->edge == ALP_GPIO_EDGE_NONE) {
		rc = ALP_OK;
	} else if (pin->state.ops->disable_irq == NULL) {
		rc = ALP_ERR_NOSUPPORT;
	} else {
		rc           = pin->state.ops->disable_irq(&pin->state);
		pin->edge    = ALP_GPIO_EDGE_NONE;
		pin->cb      = NULL;
		pin->cb_user = NULL;
	}
	alp_handle_op_leave(&pin->active_ops);
	return rc;
}

void alp_gpio_close(alp_gpio_t *pin)
{
	if (pin == NULL) return;
	/* Gate out new ops and drain any in-flight one before touching
	 * state.ops (issue #629). Losing the CAS (already closed/closing/
	 * never-opened) makes this a no-op, matching the existing
	 * void-close idempotency contract. */
	if (!alp_handle_begin_close(&pin->lifecycle, &pin->active_ops)) return;
	if (pin->state.ops != NULL && pin->state.ops->close != NULL) {
		pin->state.ops->close(&pin->state);
	}
	alp_lifecycle_set(&pin->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(pin);
}

const alp_capabilities_t *alp_gpio_capabilities(const alp_gpio_t *pin)
{
	return (pin != NULL) ? &pin->cached_caps : NULL;
}
