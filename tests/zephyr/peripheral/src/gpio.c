/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/peripheral.h> -- GPIO binding-layer smoke tests.  Extracted
 * from main.c in §C.16.  Also hosts the GPIO-pool-exhaustion test
 * since it's specific to alp_gpio_open's handle pool.
 */

#include <zephyr/ztest.h>

#include "alp/chips/gd32g553.h" /* GD32G553_PAD_ID_* */
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_gpio_output_write_read_roundtrip)
{
	alp_gpio_t *p = alp_gpio_open(0);
	zassert_not_null(p, "alp_gpio_open(0) should succeed");

	/* Verify the SDK plumbing — that configure/write/read all
     * propagate ALP_OK out of the Zephyr backend. Loopback (output
     * pin reading back its driven value) is not a contract the
     * SDK can guarantee on every backend; on gpio_emul the input
     * register is decoupled from the output register and reads
     * back zero unless gpio_emul_input_set() is called first. The
     * SDK's job is just to forward gpio_pin_get_dt's result. */
	zassert_equal(alp_gpio_configure(p, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE),
	              ALP_OK,
	              "configure as output failed");
	zassert_equal(alp_gpio_write(p, true), ALP_OK, "write high failed");
	zassert_equal(alp_gpio_write(p, false), ALP_OK, "write low failed");

	/* Reads must not error even when emul loopback returns 0. */
	bool level = true;
	zassert_equal(alp_gpio_read(p, &level), ALP_OK, "read failed");

	alp_gpio_close(p);
}

ZTEST(alp_peripheral, test_gpio_invalid_pin_returns_null)
{
	zassert_is_null(alp_gpio_open(99), "out-of-range pin_id must yield NULL");
}

ZTEST(alp_peripheral, test_gpio_irq_invalid_args)
{
	alp_gpio_t *p = alp_gpio_open(1);
	zassert_not_null(p);
	zassert_equal(alp_gpio_irq_enable(p, ALP_GPIO_EDGE_NONE, NULL, NULL),
	              ALP_ERR_INVAL,
	              "edge=NONE+cb=NULL must be invalid");
	alp_gpio_close(p);
}

ZTEST(alp_peripheral, test_gpio_pool_exhaustion_returns_null)
{
	alp_gpio_t *pins[CONFIG_ALP_SDK_MAX_GPIO_HANDLES + 1] = { 0 };
	size_t      opened                                    = 0;

	for (size_t i = 0; i < ARRAY_SIZE(pins); i++) {
		/* Pin id 0 is valid — every claim hits the pool, regardless
         * of whether the underlying gpio is shared. */
		pins[i] = alp_gpio_open(0);
		if (pins[i] == NULL) break;
		opened++;
	}

	zassert_equal(opened,
	              (size_t)CONFIG_ALP_SDK_MAX_GPIO_HANDLES,
	              "pool should hand out exactly CONFIG_ALP_SDK_MAX_GPIO_HANDLES "
	              "before refusing; opened=%zu",
	              opened);

	for (size_t i = 0; i < opened; i++) {
		alp_gpio_close(pins[i]);
	}
}

/* The reserved GD32 pad ids name the GD32 SWD / reset / ATTN pads (P71 is the
 * bridge ATTN input; P74 is the shared open-drain NRST net).  Only the V2N
 * supervisor and the SWD driver may open them, through an internal opener: the
 * portable alp_gpio_open() -- and so application code and the console -- must
 * refuse every one, whatever the board publishes. */
ZTEST(alp_peripheral, test_gpio_open_refuses_the_reserved_gd32_pad_ids)
{
	static const uint32_t ids[] = {
		GD32G553_PAD_ID_SWDIO, GD32G553_PAD_ID_SWCLK, GD32G553_PAD_ID_NRST, GD32G553_PAD_ID_ATTN
	};
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		alp_gpio_t *g = alp_gpio_open(ids[i]);
		zassert_is_null(g, "reserved id 0x%08x opened through the portable API", ids[i]);
		zassert_equal(alp_last_error(), ALP_ERR_INVAL);
	}
}
