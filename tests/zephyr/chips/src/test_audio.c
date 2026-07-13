/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Audio chip smokes: pdm_mic (block helper, v0.1 surface-only stub),
 * tas2563 (TI Class-D amp), and the v0.5 §D.audio batch mics/codecs.
 */

#include <zephyr/ztest.h>

#include "alp/blocks/pdm_mic.h"
#include "alp/chips/es8388.h"
#include "alp/chips/ics_43434.h"
#include "alp/chips/inmp441.h"
#include "alp/chips/max98357a.h"
#include "alp/chips/tas2563.h"
#include "alp/chips/tlv320aic3204.h"
#include "alp/chips/wm8960.h"
#include "alp/e1m_pinout.h"
#include "alp/peripheral.h"

/* ------------------------------------------------------------------ */
/* pdm_mic (v0.2 helper — surface only; impl returns NOSUPPORT in v0.1) */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_pdm_mic_open_returns_null_in_v01)
{
	alp_pdm_mic_t *mic = alp_pdm_mic_open(&(alp_pdm_mic_config_t){
	    .peripheral_id  = 0,
	    .sample_rate_hz = 16000,
	    .channels       = ALP_PDM_MIC_MONO,
	    .sample_bits    = 16,
	});
	zassert_is_null(mic, "v0.1 stub must return NULL until v0.2 audio lands");
}

ZTEST(alp_chips, test_pdm_mic_calls_return_nosupport)
{
	/* Even with a NULL handle (v0.1 contract), the read/set_gain
     * surface must reply ALP_ERR_NOSUPPORT — the stub asserts the
     * shape, not real arithmetic. */
	int16_t buf[16] = { 0 };
	size_t  n       = 999;
	zassert_equal(alp_pdm_mic_read(NULL, buf, sizeof buf / sizeof buf[0], &n, 0),
	              ALP_ERR_NOSUPPORT);
	zassert_equal(n, 0u, "out_frames must be zeroed by the stub");
	zassert_equal(alp_pdm_mic_set_gain(NULL, 0, 0), ALP_ERR_NOSUPPORT);
	alp_pdm_mic_close(NULL); /* must not crash. */
}

/* ------------------------------------------------------------------ */
/* tas2563 -- TI smart Class-D speaker amplifier                      */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_tas2563_init_null_args)
{
	tas2563_t  ctx;
	alp_i2c_t *bus = alp_i2c_open(&(alp_i2c_config_t){
	    .bus_id     = ALP_E1M_I2C0,
	    .bitrate_hz = 400000,
	});
	zassert_not_null(bus);

	/* sd_n is optional in the driver -- not required to be non-NULL. */
	zassert_equal(tas2563_init(NULL, bus, 0x4Du, NULL), ALP_ERR_INVAL);
	zassert_equal(tas2563_init(&ctx, NULL, 0x4Du, NULL), ALP_ERR_INVAL);

	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tas2563_calls_reject_uninitialised)
{
	tas2563_t ctx = { 0 };
	uint8_t   rev;

	zassert_equal(tas2563_read_revision(&ctx, &rev), ALP_ERR_NOT_READY);
	zassert_equal(tas2563_set_mode(&ctx, (tas2563_mode_t)0), ALP_ERR_NOT_READY);
	zassert_equal(tas2563_set_hw_enable(&ctx, true), ALP_ERR_NOT_READY);
}

/* ------------------------------------------------------------------ */
/* v0.5 §D.audio batch -- NULL-arg guard smokes                       */
/* ------------------------------------------------------------------ */

ZTEST(alp_chips, test_ics_43434_init_null_args)
{
	ics_43434_t dev;
	zassert_equal(ics_43434_init(NULL, ICS_43434_CH_LEFT), ALP_ERR_INVAL);
	zassert_equal(ics_43434_init(&dev, (ics_43434_channel_t)99), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_inmp441_init_null_args)
{
	inmp441_t dev;
	zassert_equal(inmp441_init(NULL, INMP441_CH_LEFT), ALP_ERR_INVAL);
	zassert_equal(inmp441_init(&dev, (inmp441_channel_t)99), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_wm8960_init_null_args)
{
	wm8960_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(wm8960_init(NULL, bus, WM8960_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(wm8960_init(&dev, NULL, WM8960_I2C_ADDR), ALP_ERR_INVAL);
	zassert_equal(wm8960_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_tlv320aic3204_init_null_args)
{
	tlv320aic3204_t dev;
	alp_i2c_t      *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(tlv320aic3204_init(NULL, bus, TLV320AIC3204_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(tlv320aic3204_init(&dev, NULL, TLV320AIC3204_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(tlv320aic3204_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}

ZTEST(alp_chips, test_max98357a_init_null_args)
{
	max98357a_t dev;
	zassert_equal(max98357a_init(NULL, NULL), ALP_ERR_INVAL);
	zassert_equal(max98357a_init(&dev, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_chips, test_es8388_init_null_args)
{
	es8388_t   dev;
	alp_i2c_t *bus =
	    alp_i2c_open(&(alp_i2c_config_t){ .bus_id = ALP_E1M_I2C0, .bitrate_hz = 400000 });
	zassert_not_null(bus);
	zassert_equal(es8388_init(NULL, bus, ES8388_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(es8388_init(&dev, NULL, ES8388_I2C_ADDR_LOW), ALP_ERR_INVAL);
	zassert_equal(es8388_init(&dev, bus, 0), ALP_ERR_INVAL);
	alp_i2c_close(bus);
}
