/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Test-only UART modelling the ns16550 behaviour issue #2934 depends on:
 * uart_poll_in() reads LSR, and reading LSR CLEARS the overrun/parity/framing
 * latch that uart_err_check() reports.  uart_emul's poll_in does not do that,
 * so it cannot show an error being wiped by a later poll.
 */

#define DT_DRV_COMPAT alp_test_lsr_uart

#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>

#include "lsr_uart.h"

/* Only the native_sim overlays instantiate alp,test-lsr-uart; other boards
 * (e.g. the AEN801 overlay) build this file to nothing. */
#if DT_HAS_COMPAT_STATUS_OKAY(alp_test_lsr_uart)

static struct {
	uint8_t  rx[32];
	unsigned rx_head, rx_tail;
	int      latched;     /* pending UART_ERROR_* bits */
	int      latch_after; /* latch OVERRUN after this many polls; 0 = off */
	int      polls;
	int      tx_latch_after; /* latch OVERRUN after this many poll_out calls; 0 = off */
	int      tx_polls;
} g;

void lsr_uart_reset(void)
{
	g.rx_head = g.rx_tail = 0;
	g.latched             = 0;
	g.latch_after         = 0;
	g.polls               = 0;
	g.tx_latch_after      = 0;
	g.tx_polls            = 0;
}

void lsr_uart_overrun_after_tx(int polls)
{
	g.tx_latch_after = polls;
}

void lsr_uart_push(const uint8_t *data, unsigned n)
{
	for (unsigned i = 0; i < n; i++) {
		g.rx[g.rx_tail++ % sizeof(g.rx)] = data[i];
	}
}

void lsr_uart_overrun_after_polls(int polls)
{
	g.latch_after = polls;
}

static int lsr_poll_in(const struct device *dev, unsigned char *c)
{
	ARG_UNUSED(dev);
	g.latched = 0; /* the LSR read clears OE/PE/FE */
	int rc    = -1;
	if (g.rx_head != g.rx_tail) {
		*c = g.rx[g.rx_head++ % sizeof(g.rx)];
		rc = 0;
	}
	g.polls++;
	if (g.latch_after != 0 && g.polls == g.latch_after) {
		g.latched |= UART_ERROR_OVERRUN; /* hardware overruns after this read */
	}
	return rc;
}

static void lsr_poll_out(const struct device *dev, unsigned char c)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(c);
	g.latched = 0; /* THRE spin reads LSR: clears OE/PE/FE */
	g.tx_polls++;
	if (g.tx_latch_after != 0 && g.tx_polls == g.tx_latch_after) {
		g.latched |= UART_ERROR_OVERRUN; /* hardware overruns during this TX byte */
	}
}

static int lsr_err_check(const struct device *dev)
{
	ARG_UNUSED(dev);
	int e     = g.latched;
	g.latched = 0;
	return e;
}

static DEVICE_API(uart, lsr_api) = {
	.poll_in   = lsr_poll_in,
	.poll_out  = lsr_poll_out,
	.err_check = lsr_err_check,
};

static int lsr_init(const struct device *dev)
{
	ARG_UNUSED(dev);
	return 0;
}

DEVICE_DT_INST_DEFINE(0,
                      lsr_init,
                      NULL,
                      NULL,
                      NULL,
                      POST_KERNEL,
                      CONFIG_SERIAL_INIT_PRIORITY,
                      &lsr_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(alp_test_lsr_uart) */
