/* SPDX-License-Identifier: Apache-2.0
 *
 * Unit tests for the ADC registry + vendor-extension surface.
 * Exercises the selector decision tree, capability propagation,
 * and the vendor-extension vendor-check, all on native_sim.
 *
 * Backends visible on this test build:
 *   alif_e7         (priority 100, alif:ensemble:e7)
 *   gd32_bridge     (priority 100, renesas:rzv2n:n44) -- registers
 *                    only when CONFIG_ALP_SOC_RENESAS_RZV2N_N44 is set;
 *                    not selected here (we build with ALIF=y).
 *   sw_fallback     (priority 0, "*" wildcard)
 *
 * The test build pins CONFIG_ALP_SOC_ALIF_ENSEMBLE_E7=y, so the
 * dispatcher's `alp_backend_select("adc", ALP_SOC_REF_STR)` resolves
 * to the alif_e7 backend.  Tests that need a different backend call
 * alp_backend_select directly with the desired silicon_ref string.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/ext/alif/adc.h>
#include <alp/peripheral.h>

#include "../../../../src/backends/adc/adc_ops.h"

ZTEST_SUITE(alp_adc_registry, NULL, NULL, NULL, NULL, NULL);

/* ---------- Test helper: build a fake handle pointing at a chosen backend.
 *
 * The dispatcher's real alp_adc_open requires the chosen backend to
 * actually open its ops (which fails on native_sim for the alif backend
 * because the Zephyr ADC controller isn't ready).  These tests need to
 * exercise the registry selector + vendor-check logic WITHOUT needing
 * the underlying hardware.  We construct fake handles directly. */

static struct alp_adc _fake_pool[4];
static size_t         _fake_next = 0;

static alp_adc_t *_make_fake_handle(const char *silicon_ref)
{
    if (_fake_next >= 4) {
        return NULL;
    }
    const alp_backend_t *be = alp_backend_select("adc", silicon_ref);
    if (be == NULL) {
        return NULL;
    }
    struct alp_adc *h = &_fake_pool[_fake_next++];
    memset(h, 0, sizeof(*h));
    h->backend           = be;
    h->state.ops         = (const alp_adc_ops_t *)be->ops;
    h->cached_caps.flags = be->base_caps;
    h->in_use            = true;
    return h;
}

/* ---------- Selector / priority tests ------------------------------- */

ZTEST(alp_adc_registry, test_realhw_picked_over_sw_on_alif_e7)
{
    const alp_backend_t *be =
        alp_backend_select("adc", "alif:ensemble:e7");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "alif"), 0);
    zassert_equal(be->priority, 100);
}

ZTEST(alp_adc_registry, test_sw_fallback_picked_for_unknown_silicon)
{
    /* No real backend registers for fictional silicon -- only the
     * sw_fallback wildcard matches. */
    const alp_backend_t *be =
        alp_backend_select("adc", "fictional:soc:zz");
    zassert_not_null(be);
    zassert_equal(strcmp(be->vendor, "sw"), 0);
    zassert_equal(be->priority, 0);
}

ZTEST(alp_adc_registry, test_select_returns_null_for_null_class)
{
    zassert_is_null(alp_backend_select(NULL, "alif:ensemble:e7"));
}

ZTEST(alp_adc_registry, test_select_returns_null_for_null_silicon_ref)
{
    /* Regression for the NULL silicon_ref fix in src/backend.c.
     * NULL must NOT silently match the "*" wildcard. */
    zassert_is_null(alp_backend_select("adc", NULL));
}

ZTEST(alp_adc_registry, test_backend_count_for_adc)
{
    /* alif_e7 + sw_fallback registered on this build.
     * gd32_bridge is NOT (it's CONFIG_ALP_SOC_RENESAS_RZV2N_N44-gated). */
    zassert_equal(alp_backend_count("adc"), 2u);
}

/* ---------- Public-API behaviour tests ------------------------------ */

ZTEST(alp_adc_registry, test_open_returns_inval_on_null_config)
{
    alp_adc_t *h = alp_adc_open(NULL);
    zassert_is_null(h);
}

ZTEST(alp_adc_registry, test_capabilities_returns_null_for_null_handle)
{
    zassert_is_null(alp_adc_capabilities(NULL));
}

ZTEST(alp_adc_registry, test_read_raw_inval_on_null_handle)
{
    int32_t raw = 0;
    zassert_equal(alp_adc_read_raw(NULL, &raw), ALP_ERR_INVAL);
}

ZTEST(alp_adc_registry, test_read_uv_inval_on_null_handle)
{
    int32_t uv = 0;
    zassert_equal(alp_adc_read_uv(NULL, &uv), ALP_ERR_INVAL);
}

/* ---------- Capability propagation tests --------------------------- */

ZTEST(alp_adc_registry, test_alif_handle_advertises_oversample_cap)
{
    alp_adc_t *h = _make_fake_handle("alif:ensemble:e7");
    zassert_not_null(h);
    const alp_capabilities_t *caps = alp_adc_capabilities(h);
    zassert_not_null(caps);
    zassert_true(alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_OVERSAMPLE));
    zassert_true(alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_TRIGGER));
}

ZTEST(alp_adc_registry, test_sw_handle_advertises_no_hw_caps)
{
    alp_adc_t *h = _make_fake_handle("fictional:soc:zz");
    zassert_not_null(h);
    const alp_capabilities_t *caps = alp_adc_capabilities(h);
    zassert_not_null(caps);
    zassert_false(alp_capabilities_has(caps, ALP_INSTANCE_CAP_HW_OVERSAMPLE));
    zassert_false(alp_capabilities_has(caps, ALP_INSTANCE_CAP_DMA));
}

/* ---------- Vendor-extension tests --------------------------------- */

/* set_oversampling tests removed (2026-05-22): the function was
 * dropped per the vendor-ext audit rule -- oversampling is reachable
 * via the portable alp_adc_config_t::oversampling_ratio field. */

ZTEST(alp_adc_registry, test_alif_set_trigger_rejects_non_alif_handle)
{
    alp_adc_t *h = _make_fake_handle("fictional:soc:zz");
    zassert_not_null(h);
    alp_status_t rc = alp_alif_adc_set_trigger_source(h,
                                                      ALP_ALIF_ADC_TRIGGER_TIMER0);
    zassert_equal(rc, ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
}

ZTEST(alp_adc_registry, test_alif_set_trigger_rejects_out_of_range_enum)
{
    alp_adc_t *h = _make_fake_handle("alif:ensemble:e7");
    zassert_not_null(h);
    alp_status_t rc = alp_alif_adc_set_trigger_source(h,
                                                      (alp_alif_adc_trigger_t)99);
    zassert_equal(rc, ALP_ERR_INVAL);
}

ZTEST(alp_adc_registry, test_vendor_ext_null_handle_returns_inval)
{
    zassert_equal(alp_alif_adc_set_trigger_source(NULL,
                                                   ALP_ALIF_ADC_TRIGGER_SOFTWARE),
                  ALP_ERR_INVAL);
}
