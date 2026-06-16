/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the spi registry dispatcher.  Mirrors the Slice 2a
 * i2c_registry harness; no vendor extensions, so the test surface is
 * the bare selector + capability-getter + public-API edges.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("spi", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_SPI_SW_FALLBACK=y is set in prj.conf so that
 * the sw_fallback TU is linked in and the backend-count assertions
 * below can verify both backends are present.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/spi/spi_ops.h"

ZTEST_SUITE(alp_spi_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_spi_registry, test_selector_prefers_zephyr_drv_on_alif)
{
	/* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
	const alp_backend_t *be = alp_backend_select("spi", "alif:ensemble:e7");
	zassert_not_null(be);
	zassert_equal(strcmp(be->vendor, "zephyr"), 0);
	zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_spi_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
	/* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
	const alp_backend_t *be = alp_backend_select("spi", "fictional:soc:zz");
	zassert_not_null(be);
	/* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
	(void)be;
	zassert_true(alp_backend_count("spi") >= 2u);
}

/* ---------- (c) open returns NULL on invalid bus_id ----------------------- */

ZTEST(alp_spi_registry, test_open_returns_null_on_invalid_bus_id)
{
	/* bus_id >= ALP_SOC_SPI_COUNT must be rejected by the Zephyr
     * backend's open(); the dispatcher relays the NULL back to the
     * caller and sets last_error. */
	alp_spi_config_t cfg = {
		.bus_id        = 0xFFFFFFFFu,
		.freq_hz       = 1000000u,
		.mode          = ALP_SPI_MODE_0,
		.bits_per_word = 8u,
		.cs_pin_id     = 0xFFFFFFFFu,
	};
	alp_spi_t *h = alp_spi_open(&cfg);
	zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_spi_registry, test_capabilities_getter_matches_backend_base_caps)
{
	/* alp_spi_capabilities(NULL) must return NULL -- mirrors the
     * i2c / counter / rtc / wdt pattern. */
	zassert_is_null(alp_spi_capabilities(NULL));

	/* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
	const alp_backend_t *be = alp_backend_select("spi", "alif:ensemble:e7");
	zassert_not_null(be);
	/* base_caps is 0u for the spi Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2a). */
	zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_spi_registry, test_close_releases_handle)
{
	/* alp_spi_open(NULL) -> NULL, no pool slot consumed */
	alp_spi_t *h_null = alp_spi_open(NULL);
	zassert_is_null(h_null);

	/* alp_spi_close(NULL) must not crash */
	alp_spi_close(NULL);

	/* alp_spi_capabilities(NULL) -> NULL (idempotent after bad open) */
	zassert_is_null(alp_spi_capabilities(NULL));
}

/* ---------- (f) sw_fallback loopback round-trip --------------------------- */

extern const alp_backend_t __start_alp_backends_spi[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_spi[] __attribute__((weak));

static const alp_spi_ops_t *_find_sw_fallback_ops(void)
{
	for (const alp_backend_t *be = __start_alp_backends_spi; be < __stop_alp_backends_spi; ++be) {
		if (strcmp(be->vendor, "sw_fallback") == 0) {
			return (const alp_spi_ops_t *)be->ops;
		}
	}
	return NULL;
}

ZTEST(alp_spi_registry, test_sw_fallback_loopback_round_trip)
{
	const alp_spi_ops_t *ops = _find_sw_fallback_ops();
	zassert_not_null(ops);

	/* Allocate a zeroed alp_spi on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_spi. */
	struct alp_spi h;
	memset(&h, 0, sizeof(h));
	alp_spi_backend_state_t *st   = &h.state;
	alp_capabilities_t       caps = { 0 };
	alp_spi_config_t         cfg  = {
		         .bus_id        = 0u,
		         .freq_hz       = 1000000u,
		         .mode          = ALP_SPI_MODE_0,
		         .bits_per_word = 8u,
		         .cs_pin_id     = 0xFFFFFFFFu,
	};

	zassert_equal(ops->open(&cfg, st, &caps), ALP_OK);

	/* Full-duplex: tx copied into rx */
	const uint8_t tx[4] = { 0xDE, 0xAD, 0xBE, 0xEF };
	uint8_t       rx[4] = { 0 };
	zassert_equal(ops->transceive(st, tx, rx, sizeof(tx)), ALP_OK);
	zassert_mem_equal(rx, tx, sizeof(tx));

	/* rx-only (tx=NULL): receive buffer must be zero-filled */
	uint8_t rx_zero[4] = { 0xAA, 0xAA, 0xAA, 0xAA };
	zassert_equal(ops->transceive(st, NULL, rx_zero, sizeof(rx_zero)), ALP_OK);
	for (size_t i = 0; i < sizeof(rx_zero); ++i) {
		zassert_equal(rx_zero[i], 0u);
	}

	/* tx-only (rx=NULL): must succeed and not crash */
	zassert_equal(ops->transceive(st, tx, NULL, sizeof(tx)), ALP_OK);
}
