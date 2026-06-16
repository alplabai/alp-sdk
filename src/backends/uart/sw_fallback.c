/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Software UART fallback.  Deterministic 64-byte circular byte buffer
 * for native_sim builds; not a real port.
 *
 * write(data, len) -- appends bytes into the ring; bytes that overflow
 *   the capacity are silently truncated.
 * read(data, len, timeout_ms) -- drains up to len bytes; returns as
 *   many as are available (may be zero if the ring is empty).
 *
 * Priority 0, silicon_ref="*": always loses to zephyr_drv
 * (priority 100) on real silicon; picked only when the test build
 * forces it via CONFIG_ALP_SDK_UART_SW_FALLBACK=y with no Zephyr
 * UART devices present.
 *
 * The rx_ringbuf add-on is NOT implemented by this backend.  On a
 * build where the sw_fallback is the active backend,
 * alp_uart_rx_ringbuf_attach returns NULL + ALP_ERR_NOSUPPORT via
 * the NOSUPPORT stub path in zephyr_drv.c.
 *
 * @par Cost: ROM ~400 B, RAM SW_BUF_LEN bytes (static circular byte
 *      buffer; head + tail indices).
 * @par Performance: O(len) per write/read (byte copy loop);
 *      deterministic for test assertions.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "uart_ops.h"

#define SW_BUF_LEN 64u

static uint8_t _buf[SW_BUF_LEN];
static size_t  _head  = 0u; /* read index */
static size_t  _tail  = 0u; /* write index */
static size_t  _count = 0u; /* bytes currently buffered */

static alp_status_t
sw_open(const alp_uart_config_t *cfg, alp_uart_backend_state_t *st, alp_capabilities_t *caps_out)
{
	(void)cfg;
	st->dev         = NULL;
	st->port_id     = 0u;
	st->be_data     = NULL;
	caps_out->flags = 0u;
	/* Reset the circular buffer state on each open */
	_head  = 0u;
	_tail  = 0u;
	_count = 0u;
	return ALP_OK;
}

static alp_status_t sw_write(alp_uart_backend_state_t *st, const uint8_t *data, size_t len)
{
	(void)st;
	for (size_t i = 0; i < len; i++) {
		if (_count < SW_BUF_LEN) {
			_buf[_tail] = data[i];
			_tail       = (_tail + 1u) % SW_BUF_LEN;
			_count++;
		}
		/* overflow: byte silently dropped */
	}
	return ALP_OK;
}

static alp_status_t
sw_read(alp_uart_backend_state_t *st, uint8_t *data, size_t len, uint32_t timeout_ms)
{
	(void)st;
	(void)timeout_ms;
	size_t n = (_count < len) ? _count : len;
	for (size_t i = 0; i < n; i++) {
		data[i] = _buf[_head];
		_head   = (_head + 1u) % SW_BUF_LEN;
		_count--;
	}
	return ALP_OK;
}

static const alp_uart_ops_t _ops = {
	.open  = sw_open,
	.write = sw_write,
	.read  = sw_read,
	.close = NULL,
};

ALP_BACKEND_REGISTER(uart,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
