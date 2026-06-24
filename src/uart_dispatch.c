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

#include "backends/uart/uart_ops.h"

ALP_BACKEND_DEFINE_CLASS(uart);

extern void alp_z_set_last_error(alp_status_t s);
extern void alp_z_clear_last_error(void);

#ifndef CONFIG_ALP_SDK_MAX_UART_HANDLES
#define CONFIG_ALP_SDK_MAX_UART_HANDLES 4
#endif

static struct alp_uart _pool[CONFIG_ALP_SDK_MAX_UART_HANDLES];

static struct alp_uart *_alloc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_UART_HANDLES; ++i) {
		if (!_pool[i].in_use) {
			memset(&_pool[i], 0, sizeof(_pool[i]));
			_pool[i].in_use = true;
			return &_pool[i];
		}
	}
	return NULL;
}

static void _free(struct alp_uart *h)
{
	h->in_use = false;
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
	return h;
}

alp_status_t alp_uart_write(alp_uart_t *port, const uint8_t *data, size_t len)
{
	if (port == NULL || !port->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (len == 0) return ALP_OK;
	return port->state.ops->write(&port->state, data, len);
}

alp_status_t alp_uart_read(alp_uart_t *port, uint8_t *data, size_t len, uint32_t timeout_ms)
{
	if (port == NULL || !port->in_use) return ALP_ERR_NOT_READY;
	if (data == NULL && len > 0) return ALP_ERR_INVAL;
	if (len == 0) return ALP_OK;
	return port->state.ops->read(&port->state, data, len, timeout_ms);
}

void alp_uart_close(alp_uart_t *port)
{
	if (port == NULL || !port->in_use) return;
	if (port->state.ops != NULL && port->state.ops->close != NULL) {
		port->state.ops->close(&port->state);
	}
	_free(port);
}

const alp_capabilities_t *alp_uart_capabilities(const alp_uart_t *port)
{
	return (port != NULL) ? &port->cached_caps : NULL;
}
