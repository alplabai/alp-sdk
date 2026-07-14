/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * UART (+ RX ring buffer) NOSUPPORT stubs -- <alp/peripheral.h>.
 * Split out of the former src/common/stub_backend.c monolith (issue
 * #673); owns every `alp_uart_*` symbol not provided by a vendor
 * backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#include "stub_internal.h"

#if !defined(ALP_VENDOR_OVERRIDES_UART)
alp_uart_t *alp_uart_open(const alp_uart_config_t *cfg)
{
	(void)cfg;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t alp_uart_write(alp_uart_t *p, const uint8_t *d, size_t l)
{
	(void)p;
	(void)d;
	(void)l;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_uart_read(alp_uart_t *p, uint8_t *d, size_t l, uint32_t t)
{
	(void)p;
	(void)d;
	(void)l;
	(void)t;
	return ALP_ERR_NOSUPPORT;
}
void alp_uart_close(alp_uart_t *p)
{
	(void)p;
}
#endif /* !ALP_VENDOR_OVERRIDES_UART */

/* Unguarded -- same reasoning as alp_i2c_capabilities: no Linux
 * termios wrapper or plain stub implements it; only the Zephyr registry
 * dispatcher (src/uart_dispatch.c) does (#593). */
const alp_capabilities_t *alp_uart_capabilities(const alp_uart_t *p)
{
	(void)p;
	return NULL;
}

/* UART RX ring buffer -- Zephyr-only today (CONFIG_ALP_SDK_UART_RX_RINGBUF).
 * No Yocto or baremetal backend overrides these yet, so the stubs
 * here are the canonical NOSUPPORT path on every non-Zephyr build.
 * Gated independently of ALP_VENDOR_OVERRIDES_UART so a backend can
 * adopt the ringbuf later without re-implementing the entire UART
 * surface. */
#if !defined(ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF)
alp_uart_rx_ringbuf_t *
alp_uart_rx_ringbuf_attach(alp_uart_t *port, uint8_t *backing, size_t backing_size)
{
	(void)port;
	(void)backing;
	(void)backing_size;
	z_last_error = ALP_ERR_NOSUPPORT;
	return NULL;
}
alp_status_t
alp_uart_rx_ringbuf_pop(alp_uart_rx_ringbuf_t *rb, uint8_t *out, size_t max_len, size_t *got)
{
	(void)rb;
	(void)out;
	(void)max_len;
	if (got != NULL) *got = 0;
	return ALP_ERR_NOSUPPORT;
}
size_t alp_uart_rx_ringbuf_count(const alp_uart_rx_ringbuf_t *rb)
{
	(void)rb;
	return 0;
}
void alp_uart_rx_ringbuf_detach(alp_uart_rx_ringbuf_t *rb)
{
	(void)rb;
}
#endif /* !ALP_VENDOR_OVERRIDES_UART_RX_RINGBUF */
