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

#include <stdint.h>

#include "alp/backend.h"
#include "alp/camera.h"
#include "alp/dac.h"
#include "alp/display.h"
#include "alp/dsp.h"
#include "alp/hw_info.h"
#include "alp/power.h"
#include "alp/tmu.h"
#include "alp/update_log.h"

#include "test_assert.h"

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
}

int main(void)
{
	test_every_dispatcher_links();
	test_registry_sections_are_populated();
	return 0;
}
