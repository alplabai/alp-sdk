/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * The V2N ADC backend's native burst (read_raw_n) must cost one
 * ADC_READ round trip per 8 samples, deliver the samples in order, and
 * the backend must advertise the streaming rate ceiling.
 */

#include <zephyr/ztest.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/chips/gd32g553.h>

#include "adc_ops.h"
#include "v2n_supervisor.h"

ALP_BACKEND_DEFINE_CLASS(adc);

static gd32g553_t   s_ctx;
static unsigned     s_trips;
static uint8_t      s_last_chunk;
static uint16_t     s_next_mv;
static alp_status_t s_fail_on_trip; /* status returned on trip #2 */

alp_status_t alp_z_v2n_supervisor_acquire(gd32g553_t **ctx_out)
{
	*ctx_out = &s_ctx;
	return ALP_OK;
}

void alp_z_v2n_supervisor_release(void)
{
}

alp_status_t gd32g553_adc_configure(gd32g553_t *c, uint8_t ch, uint16_t o, uint16_t sc, uint8_t r)
{
	(void)c;
	(void)ch;
	(void)o;
	(void)sc;
	(void)r;
	return ALP_OK;
}

alp_status_t gd32g553_adc_read(gd32g553_t *ctx, uint8_t channel, uint8_t samples, uint16_t *mv)
{
	(void)ctx;
	(void)channel;
	s_trips++;
	if (s_trips == 2u && s_fail_on_trip != ALP_OK) return s_fail_on_trip;
	s_last_chunk = samples;
	for (uint8_t i = 0; i < samples; ++i)
		mv[i] = s_next_mv++;
	return ALP_OK;
}

static void before(void *unused)
{
	(void)unused;
	s_trips        = 0u;
	s_next_mv      = 100u;
	s_fail_on_trip = ALP_OK;
}

ZTEST_SUITE(adc_gd32_bridge_burst, NULL, NULL, before, NULL, NULL);

static const alp_adc_ops_t *open_be(struct alp_adc *h, alp_capabilities_t *caps)
{
	const alp_backend_t *be = alp_backend_select("adc", "renesas:rzv2n:n44");
	zassert_not_null(be);
	const alp_adc_ops_t   *ops = (const alp_adc_ops_t *)be->ops;
	const alp_adc_config_t cfg = { .channel_id = 3u };
	zassert_equal(ops->open(&cfg, &h->state, caps), ALP_OK);
	return ops;
}

ZTEST(adc_gd32_bridge_burst, test_burst_chunks_by_eight)
{
	struct alp_adc       h    = { 0 };
	alp_capabilities_t   caps = { 0 };
	const alp_adc_ops_t *ops  = open_be(&h, &caps);
	int32_t              out[20];

	zassert_not_null(ops->read_raw_n);
	zassert_equal(ops->read_raw_n(&h.state, out, 20u), ALP_OK);
	zassert_equal(s_trips, 3u, "20 samples must be 8+8+4 = 3 round trips");
	zassert_equal(s_last_chunk, 4u);
	for (int i = 0; i < 20; ++i)
		zassert_equal(out[i], 100 + i);
	ops->close(&h.state);
}

ZTEST(adc_gd32_bridge_burst, test_burst_error_stops_and_propagates)
{
	struct alp_adc       h    = { 0 };
	alp_capabilities_t   caps = { 0 };
	const alp_adc_ops_t *ops  = open_be(&h, &caps);
	int32_t              out[20];

	s_fail_on_trip = ALP_ERR_IO;
	zassert_equal(ops->read_raw_n(&h.state, out, 20u), ALP_ERR_IO);
	zassert_equal(s_trips, 2u, "must stop at the failing trip");
	ops->close(&h.state);
}

ZTEST(adc_gd32_bridge_burst, test_advertises_stream_rate)
{
	struct alp_adc       h    = { 0 };
	alp_capabilities_t   caps = { 0 };
	const alp_adc_ops_t *ops  = open_be(&h, &caps);

	zassert_equal(caps.max_rate_hz, GD32G553_BRIDGE_ADC_STREAM_MAX_RATE_HZ);
	ops->close(&h.state);
}
