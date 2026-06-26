/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- UART binding-layer smoke tests.  Extracted
 * from main.c in §C.16.  Includes the UART RX-ringbuf surface
 * (alp_uart_rx_ringbuf_*) since it sits behind the same backend.
 */

#include <zephyr/ztest.h>

#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_uart_open_close_roundtrip)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){
	    .port_id   = 0,
	    .baudrate  = 115200,
	    .data_bits = 8,
	    .stop_bits = 1,
	    .parity    = ALP_UART_PARITY_NONE,
	});
	zassert_not_null(u, "alp_uart_open should succeed for port_id=0");
	alp_uart_close(u);
}

ZTEST(alp_peripheral, test_uart_invalid_port_returns_null)
{
	alp_uart_t *u = alp_uart_open(&(alp_uart_config_t){ .port_id = 99 });
	zassert_is_null(u, "out-of-range port_id must yield NULL");
}

/* UART RX ringbuf: failure paths exercised on every build.  On builds
 * without CONFIG_ALP_SDK_UART_RX_RINGBUF the attach helper returns
 * NULL with ALP_ERR_NOSUPPORT regardless of arguments.  On builds with
 * the feature on, attach validates args + returns INVAL on NULL/zero
 * inputs.  Either way the binding compiles and the failure path is
 * deterministic.  Real-IRQ attach is parked behind nightly-aen-hil. */
ZTEST(alp_peripheral, test_uart_rx_ringbuf_attach_null_port)
{
	uint8_t                buf[64];
	alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(NULL, buf, sizeof(buf));
	zassert_is_null(rb, "NULL port must yield NULL");
	/* Either ALP_ERR_INVAL (feature on) or ALP_ERR_NOSUPPORT (feature off);
     * both are valid sentinels for a refused attach.  We just assert
     * "not OK" via the non-null absence of a handle above. */
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_pop_null_fails_safely)
{
	size_t       got = 99;
	uint8_t      dst[4];
	alp_status_t s = alp_uart_rx_ringbuf_pop(NULL, dst, sizeof(dst), &got);
	/* Exact code depends on whether the build has the feature on
     * (NOT_READY) or off (NOSUPPORT).  Either is a refusal -- the
     * contract is "does not write to out, zeroes got". */
	zassert_not_equal(s, ALP_OK, "NULL handle must not return ALP_OK");
	zassert_equal(got, 0u, "got must be zeroed on failure");
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_count_null_is_zero)
{
	zassert_equal(alp_uart_rx_ringbuf_count(NULL), 0u);
}

ZTEST(alp_peripheral, test_uart_rx_ringbuf_detach_null_is_safe)
{
	/* Must not crash. */
	alp_uart_rx_ringbuf_detach(NULL);
}
