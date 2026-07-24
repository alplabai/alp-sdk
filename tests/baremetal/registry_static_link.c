/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Baremetal static-archive link regression for backend registry classes
 * not covered by the Yocto registry archive test (#368).
 *
 * This test links only against libalp_sdk.a.  It compiles no backend
 * implementation of its own, so the linker must extract each fallback
 * backend from the archive via the class anchor path.  Before the anchor
 * fix, dispatchers below can be pulled without their section-carrying
 * fallback backend, leaving __start_/__stop_alp_backends_<class>
 * unresolved or the runtime registry empty.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/backend.h"
#include "alp/camera.h"
#include "alp/dac.h"
#include "alp/display.h"
#include "alp/dsp.h"
#include "alp/hw_info.h"
#include "alp/jpeg.h"
#include "alp/power.h"
#include "alp/tmu.h"
#include "alp/update_log.h"

#include "test_assert.h"

/*
 * jpeg is the class with two always-compiled backend TUs on this build
 * (zephyr_stub.c priority 0 + sw_baseline.c priority 50, Task 2) -- the
 * static-archive anchor lives on sw_baseline.c specifically so a real
 * consumer link (this executable) pulls the backend that actually wins
 * arbitration, not the dominated stub.  Round-tripping a real encode
 * here (not just open()) is the strongest proof: if the anchor had
 * stayed on zephyr_stub.c, or sw_baseline.c were dropped from the
 * link, this would return ALP_ERR_NOT_IMPLEMENTED, not ALP_OK.
 */
static void test_jpeg_resolves_sw_baseline_not_stub(void)
{
	static const uint8_t y[16 * 16] = { 0 }, u[8 * 8] = { 0 }, v[8 * 8] = { 0 };

	alp_jpeg_config_t cfg = ALP_JPEG_CONFIG_DEFAULT;
	alp_jpeg_t       *h   = alp_jpeg_open(&cfg);
	ALP_ASSERT_TRUE(h != NULL);

	alp_jpeg_caps_t caps;
	ALP_ASSERT_EQ_INT(alp_jpeg_capabilities(h, &caps), ALP_OK);
	ALP_ASSERT_TRUE(!caps.hw_accelerated);

	uint8_t               out[4096];
	size_t                out_len = 0;
	alp_jpeg_encode_req_t req     = {
		.width     = 16,
		.height    = 16,
		.format    = ALP_PIXFMT_YUV420_PLANAR,
		.subsample = ALP_JPEG_SUBSAMPLE_420,
		.quality   = 80,
		.y_plane   = y,
		.y_stride  = 16,
		.u_plane   = u,
		.u_stride  = 8,
		.v_plane   = v,
		.v_stride  = 8,
	};
	ALP_ASSERT_EQ_INT(alp_jpeg_encode(h, &req, out, sizeof(out), &out_len), ALP_OK);
	ALP_ASSERT_TRUE(out_len > 4u);

	alp_jpeg_close(h);
}

static void test_every_dispatcher_links(void)
{
	uint16_t dac_mv = 0u;
	float    math   = 0.0f;

	alp_dac_t *dac = alp_dac_open(&(alp_dac_config_t){ .channel_id = 0u, .initial_mv = 0u });
	(void)alp_dac_read_mv(dac, &dac_mv);
	(void)alp_dac_write_mv(dac, dac_mv);
	alp_dac_close(dac);

	alp_dsp_stage_t stage = {
		.kind  = ALP_DSP_STAGE_FIR,
		.u.fir = {
			.coeff_format = ALP_DSP_COEFF_FORMAT_F32,
			.n_taps       = 1u,
			.taps         = &(const float){ 1.0f },
		},
	};
	alp_dsp_chain_t *chain = alp_dsp_chain_open(&stage, 1u);
	alp_dsp_chain_close(chain);

	alp_soc_info_t info;
	(void)alp_soc_info_read(&info);
	(void)alp_soc_secure_fw_ping();

	(void)alp_tmu_sin(0.0f, &math);

	alp_camera_t *camera = alp_camera_open(&(alp_camera_config_t){ 0 });
	(void)alp_camera_start(camera);
	alp_camera_close(camera);

	alp_display_t *display = alp_display_open(&(alp_display_config_t){ 0 });
	(void)alp_display_clear(display);
	alp_display_close(display);

	alp_power_t *power = alp_power_open();
	alp_power_close(power);

	alp_power_profile_t profile;
	(void)alp_power_profile_get(ALP_POWER_PROFILE_RUN, &profile);

	alp_update_log_t *log = alp_update_log_open();
	alp_update_log_close(log);

	ALP_ASSERT_TRUE(1);
}

static void test_registry_sections_are_populated(void)
{
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(dac));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(dsp));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(soc_info));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(tmu));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(camera));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(display));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(power));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(power_profile));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(update_log));
	ALP_ASSERT_TRUE(ALP_BACKEND_AVAILABLE(jpeg));
}

int main(void)
{
	test_every_dispatcher_links();
	test_registry_sections_are_populated();
	test_jpeg_resolves_sw_baseline_not_stub();
	ALP_TEST_SUMMARY();
}
