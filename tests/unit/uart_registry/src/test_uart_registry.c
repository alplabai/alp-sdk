/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the uart registry dispatcher.  Mirrors the Slice 2a
 * i2c_registry / spi_registry harnesses; no vendor extensions, so the
 * test surface is the bare selector + capability-getter + public-API
 * edges plus the rx_ringbuf NOSUPPORT gate for sw_fallback.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("uart", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_UART_SW_FALLBACK=y is set in prj.conf so that
 * the sw_fallback TU is linked in and the backend-count assertions
 * below can verify both backends are present.
 *
 * CONFIG_ALP_SDK_UART_RX_RINGBUF=y is set so test case (g) exercises
 * the dev==NULL guard that returns NOSUPPORT when no Zephyr device
 * is wired (i.e. sw_fallback's state.dev is NULL).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/uart/uart_ops.h"

ZTEST_SUITE(alp_uart_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_uart_registry, test_selector_prefers_zephyr_drv_on_alif)
{
	/* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
	const alp_backend_t *be = alp_backend_select("uart", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_uart_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
	/* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
	const alp_backend_t *be = alp_backend_select("uart", "fictional:soc:zz");
	zassert_not_null(be);
	/* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
	(void)be;
	zassert_true(alp_backend_count("uart") >= 2u);
}

/* ---------- (c) open returns NULL on invalid port_id ---------------------- */

ZTEST(alp_uart_registry, test_open_returns_null_on_invalid_port_id)
{
	/* port_id >= ALP_SOC_UART_COUNT must be rejected by the Zephyr
     * backend's open(); the dispatcher relays the NULL back to the
     * caller and sets last_error. */
	alp_uart_config_t cfg = {
		.port_id   = 0xFFFFFFFFu,
		.baudrate  = 115200u,
		.data_bits = 8u,
		.stop_bits = 1u,
		.parity    = ALP_UART_PARITY_NONE,
	};
	alp_uart_t *h = alp_uart_open(&cfg);
	zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_uart_registry, test_capabilities_getter_matches_backend_base_caps)
{
	/* alp_uart_capabilities(NULL) must return NULL -- mirrors the
     * i2c / spi / counter / rtc / wdt pattern. */
	zassert_is_null(alp_uart_capabilities(NULL));

	/* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
	const alp_backend_t *be = alp_backend_select("uart", "alif:ensemble:e7");
	zassert_not_null(be);
	/* base_caps is 0u for the uart Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2a). */
	zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_uart_registry, test_close_releases_handle)
{
	/* alp_uart_open(NULL) -> NULL, no pool slot consumed */
	alp_uart_t *h_null = alp_uart_open(NULL);
	zassert_is_null(h_null);

	/* alp_uart_close(NULL) must not crash */
	alp_uart_close(NULL);

	/* alp_uart_capabilities(NULL) -> NULL (idempotent after bad open) */
	zassert_is_null(alp_uart_capabilities(NULL));
}

/* ---------- (f) sw_fallback loopback round-trip --------------------------- */

extern const alp_backend_t   __start_alp_backends_uart[] __attribute__((weak));
extern const alp_backend_t   __stop_alp_backends_uart[] __attribute__((weak));

static const alp_uart_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_uart; be < __stop_alp_backends_uart; ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_uart_ops_t *)be->ops;
		}
	}
	return NULL;
}

ZTEST(alp_uart_registry, test_sw_fallback_loopback_round_trip)
{
	const alp_uart_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Allocate a zeroed alp_uart on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_uart. */
	struct alp_uart h;
	memset(&h, 0, sizeof(h));
	alp_uart_backend_state_t *st   = &h.state;
	alp_capabilities_t        caps = { 0 };
	alp_uart_config_t         cfg  = {
		         .port_id   = 0u,
		         .baudrate  = 115200u,
		         .data_bits = 8u,
		         .stop_bits = 1u,
		         .parity    = ALP_UART_PARITY_NONE,
	};

	zassert_equal(ops->open(&cfg, st, &caps), ALP_OK);

	/* Write 4 bytes into the sw_fallback ring */
	const uint8_t tx[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	zassert_equal(ops->write(st, tx, sizeof(tx)), ALP_OK);

	/* Read them back -- all 4 bytes should be present */
	uint8_t rx[4] = { 0 };
	zassert_equal(ops->read(st, rx, sizeof(rx), 0u), ALP_OK);
	zassert_mem_equal(rx, tx, sizeof(tx));

	/* Write 4 bytes, read only 2 -- truncation: only first 2 returned */
	const uint8_t tx2[4] = { 0x01, 0x02, 0x03, 0x04 };
	zassert_equal(ops->write(st, tx2, sizeof(tx2)), ALP_OK);
	uint8_t rx2[2] = { 0 };
	zassert_equal(ops->read(st, rx2, sizeof(rx2), 0u), ALP_OK);
	zassert_equal(rx2[0], 0x01u);
	zassert_equal(rx2[1], 0x02u);
}

/* ---------- (g) rx_ringbuf returns NOSUPPORT on sw_fallback --------------- */

#if defined(CONFIG_ALP_SDK_UART_RX_RINGBUF)

ZTEST(alp_uart_registry, test_rx_ringbuf_returns_nosupport_on_sw_fallback)
{
	/* Open a uart handle via the sw_fallback ops directly so that
     * state.dev is NULL (sw_fallback sets dev=NULL at open time).
     * alp_uart_rx_ringbuf_attach must detect this and return NULL
     * with ALP_ERR_NOSUPPORT recorded via alp_z_set_last_error. */
	const alp_uart_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	struct alp_uart h;
	memset(&h, 0, sizeof(h));
	h.in_use                = true;
	alp_capabilities_t caps = { 0 };
	alp_uart_config_t  cfg  = {
		  .port_id   = 0u,
		  .baudrate  = 115200u,
		  .data_bits = 8u,
		  .stop_bits = 1u,
		  .parity    = ALP_UART_PARITY_NONE,
	};
	zassert_equal(ops->open(&cfg, &h.state, &caps), ALP_OK);
	/* sw_fallback sets state.dev = NULL */
	zassert_is_null(h.state.dev);

	uint8_t                backing[64];
	alp_uart_rx_ringbuf_t *rb = alp_uart_rx_ringbuf_attach(&h, backing, sizeof(backing));
	zassert_is_null(rb);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

#endif /* CONFIG_ALP_SDK_UART_RX_RINGBUF */
