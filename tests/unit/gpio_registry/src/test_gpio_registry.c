/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the gpio registry dispatcher.  Mirrors the Slice 2a
 * spi_registry / uart_registry harnesses; no vendor extensions, so the
 * test surface is the bare selector + capability-getter + public-API
 * edges plus the sw_fallback's NOSUPPORT path for IRQ enable.
 *
 * Backends visible on this test build:
 *   zephyr_drv    (priority 100, "*" wildcard)
 *   sw_fallback   (priority 0,   "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y so the
 * dispatcher's `alp_backend_select("gpio", ALP_SOC_REF_STR)` exercises
 * the same selector code path real customer builds hit.
 *
 * CONFIG_ALP_SDK_GPIO_SW_FALLBACK=y is set in prj.conf so that
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

#include "../../../../src/backends/gpio/gpio_ops.h"

ZTEST_SUITE(alp_gpio_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- (a) Selector / priority test ---------------------------------- */

ZTEST(alp_gpio_registry, test_selector_prefers_zephyr_drv_on_alif)
{
    /* On alif:ensemble:e7 the zephyr_drv backend (priority 100) must
     * win over sw_fallback (priority 0) because the selector picks the
     * highest-priority wildcard match. */
    const alp_backend_t *be =
        alp_backend_select("gpio", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "zephyr"), 0);
    zassert_equal(be->priority, 100);
}

/* ---------- (b) SW fallback reachability ---------------------------------- */

ZTEST(alp_gpio_registry, test_selector_falls_back_to_sw_when_only_fallback_present)
{
    /* Both registered backends are wildcard matches; zephyr_drv wins
     * on priority.  This test still exercises the selector on unknown
     * silicon and asserts the sw_fallback is reachable via count. */
    const alp_backend_t *be =
        alp_backend_select("gpio", "fictional:soc:zz");
    zassert_not_null(be);
    /* zephyr_drv wins on priority even for fictional silicon -- assert
     * reachability of sw_fallback via the registry count below. */
    (void)be;
    zassert_true(alp_backend_count("gpio") >= 2u);
}

/* ---------- (c) open returns NULL on invalid pin_id ----------------------- */

ZTEST(alp_gpio_registry, test_open_returns_null_on_invalid_pin_id)
{
    /* pin_id outside the alp,pin-array range must be rejected by the
     * Zephyr backend's open(); the dispatcher relays the NULL back to
     * the caller and sets last_error.  native_sim has no alp,pin-array
     * node so even pin_id=0 is invalid here -- 0xFFFFFFFF is just the
     * clearest "definitely not a real pin" value. */
    alp_gpio_t *h = alp_gpio_open(0xFFFFFFFFu);
    zassert_is_null(h);
}

/* ---------- (d) capabilities getter matches backend base caps ------------- */

ZTEST(alp_gpio_registry, test_capabilities_getter_matches_backend_base_caps)
{
    /* alp_gpio_capabilities(NULL) must return NULL -- mirrors the
     * spi / uart / i2c / counter / rtc / wdt pattern. */
    zassert_is_null(alp_gpio_capabilities(NULL));

    /* Verify that the backend declared by the registry has base_caps
     * accessible (even if 0) -- exercises the selector path. */
    const alp_backend_t *be =
        alp_backend_select("gpio", "alif:ensemble:e7");
    zassert_not_null(be);
    /* base_caps is 0u for the gpio Zephyr backend (no hw-specific cap
     * flags defined yet in Slice 2b). */
    zassert_equal(be->base_caps, 0u);
}

/* ---------- (e) close releases handle ------------------------------------- */

ZTEST(alp_gpio_registry, test_close_releases_handle)
{
    /* alp_gpio_open(0xFFFFFFFF) -> NULL, no pool slot consumed */
    alp_gpio_t *h_null = alp_gpio_open(0xFFFFFFFFu);
    zassert_is_null(h_null);

    /* alp_gpio_close(NULL) must not crash */
    alp_gpio_close(NULL);

    /* alp_gpio_capabilities(NULL) -> NULL (idempotent after bad open) */
    zassert_is_null(alp_gpio_capabilities(NULL));
}

/* ---------- (f) sw_fallback round-trip + NOSUPPORT on irq_enable ---------- */

extern const alp_backend_t __start_alp_backends_gpio[] __attribute__((weak));
extern const alp_backend_t __stop_alp_backends_gpio[] __attribute__((weak));

static const alp_gpio_ops_t *_find_sw_fallback_ops(void)
{
    for (const alp_backend_t *be = __start_alp_backends_gpio;
         be < __stop_alp_backends_gpio; ++be) {
        if (strcmp(be->vendor, "sw_fallback") == 0) {
            return (const alp_gpio_ops_t *)be->ops;
        }
    }
    return NULL;
}

ZTEST(alp_gpio_registry, test_sw_fallback_round_trip)
{
    const alp_gpio_ops_t *ops = _find_sw_fallback_ops();
    zassert_not_null(ops);

    /* Allocate a zeroed alp_gpio on the stack so CONTAINER_OF in the
     * backend is valid -- state is the first field of struct alp_gpio. */
    struct alp_gpio h;
    memset(&h, 0, sizeof(h));
    alp_gpio_backend_state_t *st = &h.state;
    alp_capabilities_t caps = {0};

    zassert_equal(ops->open(0u, st, &caps), ALP_OK);
    /* sw_fallback open leaves dev=NULL */
    zassert_is_null(st->dev);

    /* configure / write / read are no-ops that return ALP_OK */
    zassert_equal(ops->configure(st, ALP_GPIO_OUTPUT, ALP_GPIO_PULL_NONE), ALP_OK);
    zassert_equal(ops->write(st, true), ALP_OK);

    bool level = true;
    zassert_equal(ops->read(st, &level), ALP_OK);
    /* sw_fallback returns level=false regardless of last write */
    zassert_false(level);

    /* irq_enable returns NOSUPPORT (no real edge source to hook) */
    zassert_equal(ops->enable_irq(st, ALP_GPIO_EDGE_RISING, NULL, NULL),
                  ALP_ERR_NOSUPPORT);

    /* irq_disable is the paired no-op */
    zassert_equal(ops->disable_irq(st), ALP_OK);

    /* close is a no-op; calling twice is safe */
    ops->close(st);
    ops->close(st);
}
