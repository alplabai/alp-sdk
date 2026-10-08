/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * <alp/i3c.h> -- I3C wrapper tests.  Modeled on src/dac.c.  Covers
 * NULL-cfg INVAL, out-of-range bus_id, the NOT_READY-or-NOSUPPORT
 * open path when no underlying controller is wired, NULL-handle
 * behaviour on every blocking op, close(NULL) idempotency, and
 * capabilities(NULL).
 *
 * NULL-data/nonzero-len INVAL (the dispatcher's
 * `data == NULL && len > 0` guard in src/i3c_dispatch.c) needs an
 * OPENED handle to reach -- on every Zephyr build (native_sim
 * included) alp_i3c_open() always resolves to the zephyr_drv backend
 * (priority 100, "*") over sw_fallback (priority 0, "*"; see
 * alp_backend_select's tiebreak in src/backend.c), and native_sim has
 * no alp-i3c0 DT alias, so open() always returns NULL here -- there
 * is no reachable opened handle on this backend combination to drive
 * that guard through the public dispatcher.  Not tested for that
 * reason (matches the sibling I2C/DAC ztest suites, which don't cover
 * it either).
 */

#include <zephyr/ztest.h>

#include "alp/i3c.h" /* alp_i3c_open / alp_i3c_t / alp_i3c_config_t */
#include "alp/peripheral.h"

ZTEST(alp_peripheral, test_i3c_null_cfg)
{
	zassert_is_null(alp_i3c_open(NULL));
	zassert_equal(alp_last_error(), ALP_ERR_INVAL);
}

ZTEST(alp_peripheral, test_i3c_out_of_range_bus)
{
	/* Bus id 9 is out of range for any E1M part (ALP_E1M_I3C_COUNT = 1).
	 *
	 * Admission is the backend registry, not the SoC cap table (issue
	 * #1642's dispatch-level `ALP_SOC_I3C_COUNT > 0` gate is gone): a
	 * backend registered on a SoC with ALP_SOC_I3C_COUNT == 0 can still
	 * serve a bus, so alp_i3c_open() no longer rejects up front on the
	 * cap table.  Which backend wins (zephyr_drv here, no
	 * CONFIG_I3C_CONTROLLER in this suite) decides the code -- NOSUPPORT
	 * if the backend declines outright, INVAL if it bounds-checks the
	 * bus itself.  Either way the bus is out of range and open() returns
	 * NULL. */
	alp_i3c_t *b = alp_i3c_open(&(alp_i3c_config_t){ .bus_id = 9u });

	zassert_is_null(b);
}

ZTEST(alp_peripheral, test_i3c_unresolved_bus_yields_not_ready)
{
	/* Without a real I3C controller backing bus 0 (no alp-i3c0 DT alias
	 * on native_sim), open must fail with NOT_READY (DT-alias path) or
	 * NOSUPPORT (CONFIG_I3C=n).  Either is acceptable; both surface as
	 * a NULL return. */
	alp_i3c_t *b = alp_i3c_open(&(alp_i3c_config_t){ .bus_id = 0u });

	zassert_is_null(b);
}

ZTEST(alp_peripheral, test_i3c_write_on_null_handle_errors)
{
	alp_status_t s = alp_i3c_write(NULL, 0x08u, (uint8_t[]){ 0xaa }, 1u);

	zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}

ZTEST(alp_peripheral, test_i3c_read_on_null_handle_errors)
{
	uint8_t      byte = 0u;
	alp_status_t s    = alp_i3c_read(NULL, 0x08u, &byte, 1u);

	zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}

ZTEST(alp_peripheral, test_i3c_write_read_on_null_handle_errors)
{
	uint8_t      reg = 0u, val = 0u;
	alp_status_t s = alp_i3c_write_read(NULL, 0x08u, &reg, 1u, &val, 1u);

	zassert_equal(s, ALP_ERR_NOT_READY, "got %d", (int)s);
}

ZTEST(alp_peripheral, test_i3c_close_null_is_noop)
{
	/* Must not crash; idempotent on NULL per the header contract. */
	alp_i3c_close(NULL);
}

ZTEST(alp_peripheral, test_i3c_capabilities_null_handle_returns_null)
{
	zassert_is_null(alp_i3c_capabilities(NULL));
}
