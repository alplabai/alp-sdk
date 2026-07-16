/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Secure-element chip smokes: optiga_trust_m (Infineon), atecc608b
 * (Microchip, v0.5 §D.iot batch).
 */

#include <zephyr/ztest.h>

#include "alp/chips/atecc608b.h"
#include "alp/chips/optiga_trust_m.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* optiga_trust_m -- Infineon secure element                          */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_optiga_trust_m_init_null_args)
{
	optiga_trust_m_t ctx;
	alp_i2c_t       *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_equal(optiga_trust_m_init(NULL, bus, OPTIGA_TRUST_M_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(optiga_trust_m_init(&ctx, NULL, OPTIGA_TRUST_M_I2C_ADDR), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

/* #739: no fixed strap range is documented for Trust M (provisioning-
 * defined address), so only the generic 7-bit domain bound applies;
 * 0 stays the documented "use provisioned default" sentinel. */
ZTEST(alp_chips, test_optiga_trust_m_init_validates_7bit_address_bound)
{
	optiga_trust_m_t ctx;
	alp_i2c_t       *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	zassert_not_equal(optiga_trust_m_init(&ctx, bus, 0x00u),
	                  ALP_ERR_INVAL,
	                  "addr=0 is the documented default-address sentinel, must not be INVAL");
	zassert_not_equal(optiga_trust_m_init(&ctx, bus, 0x7Fu),
	                  ALP_ERR_INVAL,
	                  "0x7F is the last valid 7-bit address");

	zassert_equal(
	    optiga_trust_m_init(&ctx, bus, 0x80u), ALP_ERR_INVAL, "0x80 exceeds 7-bit domain");
	zassert_equal(
	    optiga_trust_m_init(&ctx, bus, 0xFFu), ALP_ERR_INVAL, "0xFF exceeds 7-bit domain");

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_optiga_trust_m_calls_reject_uninitialised)
{
	optiga_trust_m_t              ctx = { 0 };
	optiga_trust_m_product_info_t info;
	uint8_t                       apdu[8] = { 0 };
	uint8_t                       resp[16];
	size_t                        resp_len;

	zassert_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_ERR_NOT_READY);
	zassert_equal(
	    optiga_trust_m_send_apdu(&ctx, apdu, sizeof apdu, resp, sizeof resp, &resp_len, 100u),
	    ALP_ERR_NOT_READY);

	optiga_trust_m_deinit(&ctx);
	optiga_trust_m_deinit(NULL);
}

ZTEST(alp_chips, test_optiga_trust_m_probe_only_api_contract)
{
	optiga_trust_m_t              ctx = { .initialised = true };
	optiga_trust_m_product_info_t info;
	uint8_t                       apdu[4] = { 0x31u, 0x11u, 0x00u, 0x00u };
	uint8_t                       resp[8];
	size_t                        resp_len = 123u;

	zassert_equal(optiga_trust_m_read_product_info(&ctx, NULL), ALP_ERR_INVAL);
	zassert_equal(optiga_trust_m_read_product_info(&ctx, &info), ALP_ERR_NOSUPPORT);

	zassert_equal(
	    optiga_trust_m_send_apdu(&ctx, NULL, sizeof apdu, resp, sizeof resp, &resp_len, 100u),
	    ALP_ERR_INVAL);
	zassert_equal(optiga_trust_m_send_apdu(&ctx, apdu, 0u, resp, sizeof resp, &resp_len, 100u),
	              ALP_ERR_INVAL);
	zassert_equal(
	    optiga_trust_m_send_apdu(&ctx, apdu, sizeof apdu, NULL, sizeof resp, &resp_len, 100u),
	    ALP_ERR_INVAL);
	zassert_equal(optiga_trust_m_send_apdu(&ctx, apdu, sizeof apdu, resp, 0u, &resp_len, 100u),
	              ALP_ERR_INVAL);
	zassert_equal(optiga_trust_m_send_apdu(&ctx, apdu, sizeof apdu, resp, sizeof resp, NULL, 100u),
	              ALP_ERR_INVAL);

	resp_len = 123u;
	zassert_equal(
	    optiga_trust_m_send_apdu(&ctx, apdu, sizeof apdu, resp, sizeof resp, &resp_len, 100u),
	    ALP_ERR_NOSUPPORT);
	zassert_equal(resp_len, 0u);
}

/* ------------------------------------------------------------------ */
/* atecc608b -- v0.5 §D.iot batch secure element                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_atecc608b_init_null_args)
{
	atecc608b_t dev;
	alp_i2c_t  *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(atecc608b_init(NULL, bus, ATECC608B_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(atecc608b_init(&dev, NULL, ATECC608B_I2C_ADDR_DEFAULT), ALP_ERR_INVAL);
	zassert_equal(atecc608b_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}
