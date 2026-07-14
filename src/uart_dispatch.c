/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART class dispatcher.  Routes the public alp_uart_* API
 * through the .alp_backends_uart registry.
 *
 * The alp_uart_rx_ringbuf_* family is declared by <alp/peripheral.h>
 * but its bodies live in src/backends/uart/zephyr_drv.c -- they are
 * Zephyr driver-class-specific (uart_irq_callback_set /
 * uart_irq_rx_enable) and do not enter the ops vtable.  The
 * sw_fallback backend returns NULL + ALP_ERR_NOSUPPORT for attach.
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
#include "backends/uart/uart_ops.h"

ALP_BACKEND_DEFINE_CLASS(uart);

#include "alp_z_last_error.h"

#ifndef CONFIG_ALP_SDK_MAX_UART_HANDLES
#define CONFIG_ALP_SDK_MAX_UART_HANDLES 4
#endif

static struct alp_uart _pool[CONFIG_ALP_SDK_MAX_UART_HANDLES];

static struct alp_uart *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_UART_HANDLES; ++i) {
		/* Atomic claim (issue #629): only the winner of the flag flip
		 * may touch the slot's other fields -- in_use is the
		 * struct's last member, so zero everything before it. */
		if (alp_slot_try_claim(&_pool[i].in_use)) {
			memset(&_pool[i], 0, offsetof(struct alp_uart, in_use));
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_uart *h)
{
	alp_slot_release(&h->in_use);
}

alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL) {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("uart", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_uart_ops_t *ops = (const alp_uart_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_uart *h = _alloc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend              = be;
	h->state.ops            = ops;
	alp_capabilities_t caps = { .flags = be->base_caps };
	if (be->probe != NULL) {
		uint32_t refined = caps.flags;
		(void)be->probe(cfg->port_id, &refined);
		caps.flags = refined;
	}
	alp_status_t rc = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	alp_lifecycle_set(&h->lifecycle, ALP_HANDLE_LC_OPEN);
	return h;
}

alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len)
{
	/* Gate on the lifecycle byte, not in_use -- in_use is now touched
	 * only by the atomic claim/release in _alloc/_free (issue #629). */
	if (port == NULL || !alp_handle_op_enter(&port->lifecycle, &port->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (data == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (len == 0) {
		rc = ALP_OK;
	} else {
		rc = port->state.ops->write(&port->state, data, len);
	}
	alp_handle_op_leave(&port->active_ops);
	return rc;
}

alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (port == NULL || !alp_handle_op_enter(&port->lifecycle, &port->active_ops)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (data == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (len == 0) {
		rc = ALP_OK;
	} else {
		rc = port->state.ops->read(&port->state, data, len, timeout_ms);
	}
	alp_handle_op_leave(&port->active_ops);
	return rc;
}

void alp_uart_close(alp_uart_t *port)
{
	if (port == NULL) return;
	/* Sleep-poll drain (issue #629 follow-up): this pool counts
	 * alp_uart_read(), which can block up to its caller's timeout_ms, so
	 * alp_handle_begin_close_blocking() sleeps between polls instead of
	 * busy-spinning -- the busy-spin alp_handle_begin_close() would peg
	 * a core (or hang outright at timeout_ms == UINT32_MAX) for the
	 * whole read. See src/common/alp_slot_claim.c/.h. Idempotent: a
	 * second/never-opened close no-ops. */
	if (!alp_handle_begin_close_blocking(&port->lifecycle, &port->active_ops)) return;
	if (port->state.ops != NULL && port->state.ops->close != NULL) {
		port->state.ops->close(&port->state);
	}
	alp_lifecycle_set(&port->lifecycle, ALP_HANDLE_LC_UNOPENED);
	_free(port);
}

const alp_capabilities_t *alp_uart_capabilities(const alp_uart_t *port)
{
	return (port != NULL) ? &port->cached_caps : NULL;
}
