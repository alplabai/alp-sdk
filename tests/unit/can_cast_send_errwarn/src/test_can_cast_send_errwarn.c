/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2983: can_cast_send() refused every frame once the error-warning
 * limit was reached.  Error-warning (and error-passive) controllers still
 * transmit; only bus-off is refused (-ENETUNREACH, Zephyr can_send()
 * contract).
 */
#pragma GCC diagnostic ignored "-Wunused-function"
/* The driver holds MMIO addresses in uint32_t (32-bit SoC); the test maps its
 * register page below 4 GiB and the casts are harmless there. */
#pragma GCC diagnostic ignored "-Wint-to-pointer-cast"
#include "../../../../zephyr/drivers/can/can_cast.c"

#include <sys/mman.h>

#include <zephyr/ztest.h>

static struct can_cast_config g_cfg;
static struct can_cast_data   g_data;
static struct device          g_dev = { .config = &g_cfg, .data = &g_data };

static uint8_t *g_regs;

static void before(void *fixture)
{
	(void)fixture;
	if (g_regs == NULL) {
		/* The driver keeps the register base in a uint32_t, so the page
		 * must live in the low 4 GiB of the 64-bit host. */
		g_regs = mmap(
		    NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
		zassert_not_equal(g_regs, MAP_FAILED);
	}
	memset(g_regs, 0, 4096);
	memset(&g_data, 0, sizeof(g_data));
	*(mm_reg_t *)&DEVICE_MMIO_NAMED_ROM_PTR(&g_dev, can_reg)->addr = (mm_reg_t)g_regs;
	k_mutex_init(&g_data.inst_mutex);
	g_data.common.started = true;
	g_data.err_state      = CAN_STATE_ERROR_ACTIVE;
}

ZTEST_SUITE(can_cast_send_errwarn, NULL, NULL, before, NULL, NULL);

static void tx_cb(const struct device *dev, int error, void *arg)
{
	ARG_UNUSED(dev);
	ARG_UNUSED(error);
	ARG_UNUSED(arg);
}

static int send_one(void)
{
	struct can_frame frame = { .id = 0x123, .dlc = 1, .data = { 0x55 } };

	return can_cast_send(&g_dev, &frame, K_NO_WAIT, tx_cb, NULL);
}

ZTEST(can_cast_send_errwarn, test_send_ok_at_error_warning_limit)
{
	g_regs[CAN_ERRINT] = BIT(CAN_ERRINT_EWARN); /* EWARN set */
	g_data.err_state   = CAN_STATE_ERROR_WARNING;

	zassert_equal(send_one(), 0, "error-warning is not bus-off: the frame must be accepted");
}

ZTEST(can_cast_send_errwarn, test_send_refused_in_bus_off)
{
	g_data.err_state = CAN_STATE_BUS_OFF;

	zassert_equal(send_one(), -ENETUNREACH);
}
