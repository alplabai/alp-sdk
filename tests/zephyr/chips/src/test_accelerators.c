/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * AI-accelerator bring-up chip smokes: deepx_dxm1 (DEEPX DX-M1 NPU
 * host-side sequencer), pi3dbs12212 (the PCIe mux deepx_dxm1 drives
 * directly), and hailo_8l.
 */

#include <zephyr/ztest.h>

#include "alp/chips/deepx_dxm1.h"
#include "alp/chips/hailo_8l.h"
#include "alp/chips/pi3dbs12212.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* deepx_dxm1 -- DEEPX DX-M1 NPU host-side bring-up sequencer         */
/*                                                                    */
/* The driver consumes two opened handles:                            */
/*   - alp_gpio_t for M1_RESET (Renesas PA6 on V2N-M1)                */
/*   - pi3dbs12212_t mux context                                       */
/* The mux context itself takes two alp_gpio_t pinned to PD + SEL.     */
/* For the NULL-arg coverage we only need the validation rejections   */
/* to fire; for the post-init-rejection tests we use a zeroed ctx.    */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_deepx_dxm1_init_null_args)
{
	deepx_dxm1_t ctx;
	/* All three pointer args must be non-NULL.  The validation order is
     * (ctx, m1_reset, pcie_mux) — any single NULL is rejected.  We don't
     * need to construct a real mux handle since the function returns
     * INVAL before it dereferences any of them. */
	pi3dbs12212_t bogus_mux = { 0 };
	alp_gpio_t   *bogus_pin = (alp_gpio_t *)0xDEADBEEFu;

	zassert_equal(deepx_dxm1_init(NULL, bogus_pin, &bogus_mux, PI3DBS_STATE_PATH_0),
	              ALP_ERR_INVAL,
	              "NULL ctx must be rejected");
	zassert_equal(deepx_dxm1_init(&ctx, NULL, &bogus_mux, PI3DBS_STATE_PATH_0),
	              ALP_ERR_INVAL,
	              "NULL m1_reset must be rejected");
	zassert_equal(deepx_dxm1_init(&ctx, bogus_pin, NULL, PI3DBS_STATE_PATH_0),
	              ALP_ERR_INVAL,
	              "NULL mux ctx must be rejected");

	/* Out-of-range deepx_path enum value -- caller must hit one of
     * PI3DBS_STATE_PATH_0 or PI3DBS_STATE_PATH_1.  PI3DBS_STATE_OFF
     * isn't a valid "to DEEPX" destination. */
	zassert_equal(deepx_dxm1_init(&ctx, bogus_pin, &bogus_mux, PI3DBS_STATE_OFF),
	              ALP_ERR_INVAL,
	              "deepx_path = OFF is not a valid bring-up destination");
}

ZTEST(alp_chips, test_deepx_dxm1_bring_up_rejects_uninitialised)
{
	deepx_dxm1_t ctx = { 0 };
	/* The sequencer must report NOT_READY rather than dereferencing
     * NULL m1_reset_pin / pcie_mux when called on a zeroed context. */
	zassert_equal(deepx_dxm1_bring_up(&ctx, 0u), ALP_ERR_NOT_READY);
	zassert_equal(deepx_dxm1_shut_down(&ctx), ALP_ERR_NOT_READY);
	zassert_equal(deepx_dxm1_set_reset_polarity(&ctx, DEEPX_DXM1_RESET_ACTIVE_HIGH),
	              ALP_ERR_NOT_READY);

	/* deinit on a zero context must be safe. */
	deepx_dxm1_deinit(&ctx);
	deepx_dxm1_deinit(NULL);
}

ZTEST(alp_chips, test_deepx_dxm1_set_reset_polarity_invalid_value)
{
	/* Force initialised so the function reaches the polarity-range
     * check before the m1_reset_pin write -- m1_reset_pin is NULL
     * here but the check rejects on the value first. */
	deepx_dxm1_t ctx = { .initialised = true };

	/* Pass a value outside the documented enum range -- both LOW (0)
     * and HIGH (1) are valid; 2 is not. */
	zassert_equal(deepx_dxm1_set_reset_polarity(&ctx, (deepx_dxm1_reset_polarity_t)2),
	              ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* pi3dbs12212 -- passive 2:1 PCIe mux                                */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pi3dbs12212_init_null_args)
{
	pi3dbs12212_t ctx;
	alp_gpio_t   *bogus = (alp_gpio_t *)0xDEADBEEFu;

	zassert_equal(pi3dbs12212_init(NULL, bogus, bogus), ALP_ERR_INVAL);
	zassert_equal(pi3dbs12212_init(&ctx, NULL, bogus), ALP_ERR_INVAL);
	zassert_equal(pi3dbs12212_init(&ctx, bogus, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_pi3dbs12212_calls_reject_uninitialised)
{
	pi3dbs12212_t       ctx = { 0 };
	pi3dbs12212_state_t state;

	zassert_equal(pi3dbs12212_set_state(&ctx, PI3DBS_STATE_PATH_0), ALP_ERR_NOT_READY);
	zassert_equal(pi3dbs12212_get_state(&ctx, &state), ALP_ERR_NOT_READY);

	/* deinit on a zero ctx is a no-op. */
	pi3dbs12212_deinit(&ctx);
	pi3dbs12212_deinit(NULL);
}

ZTEST(alp_chips, test_pi3dbs12212_set_state_invalid_value)
{
	/* Force .initialised so the function reaches the enum check.
     * pd / sel are NULL here but the check rejects on the value
     * first. */
	pi3dbs12212_t ctx = { .initialised = true };

	/* PI3DBS_STATE_OFF / PATH_0 / PATH_1 are 0 / 1 / 2; 3 isn't
     * valid. */
	zassert_equal(pi3dbs12212_set_state(&ctx, (pi3dbs12212_state_t)3), ALP_ERR_INVAL);
}

/* ------------------------------------------------------------------ */
/* hailo_8l -- v0.5 §D.AI batch accelerator                            */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_hailo_8l_init_null_args)
{
	hailo_8l_t dev;
	zassert_equal(hailo_8l_init(NULL, NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(hailo_8l_init(&dev, NULL, NULL), ALP_ERR_INVAL);
}
