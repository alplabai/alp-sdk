/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>
#include <string.h>
#include <alp/inference.h>
#include <alp/peripheral.h>
#include "../../../../src/backends/inference/alp_model_select.h"

/* helper: build a target */
static alp_model_target_t T(const char    *be,
                            const char    *sil,
                            const char    *fmt,
                            uint32_t       arena,
                            uint32_t       sram,
                            const uint8_t *blob,
                            uint32_t       blen)
{
	alp_model_target_t t = { 0 };
	strncpy(t.backend, be, ALP_MODEL_STR_MAX - 1);
	strncpy(t.silicon_ref, sil, ALP_MODEL_STR_MAX - 1);
	strncpy(t.blob_format, fmt, ALP_MODEL_STR_MAX - 1);
	t.arena_bytes  = arena;
	t.req_sram_kib = sram;
	t.blob         = blob;
	t.blob_len     = blen;
	return t;
}

ZTEST_SUITE(alp_model_select, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_model_select, test_auto_picks_matching_npu_over_cpu)
{
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m = { 0 };
	m.n_targets            = 2;
	m.targets[0]           = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 65536, 256, b0, 4);
	m.targets[1]           = T("cpu", "*", "tflite", 131072, 0, b1, 4);
	const char               *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t    env     = { .soc_ref           = "alif:ensemble:e7",
		                                  .avail_silicon     = avail,
		                                  .n_avail_silicon   = 1,
		                                  .arena_sram_kib    = 4096,
		                                  .preferred_backend = ALP_INFERENCE_BACKEND_AUTO };
	alp_model_select_result_t r       = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.target_index, 0u);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_ETHOS_U);
	zassert_equal(r.format, ALP_INFERENCE_MODEL_VELA);
	zassert_equal(r.arena_bytes, 65536u);
}

ZTEST(alp_model_select, test_no_fit_falls_back_to_cpu)
{
	/* ethos_u blob needs 512 KiB but device budget is 256 KiB -> CPU. */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m = { 0 };
	m.n_targets            = 2;
	m.targets[0]           = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 0, 512, b0, 4);
	m.targets[1]           = T("cpu", "*", "tflite", 0, 0, b1, 4);
	const char               *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t    env     = { .soc_ref           = "alif:ensemble:e7",
		                                  .avail_silicon     = avail,
		                                  .n_avail_silicon   = 1,
		                                  .arena_sram_kib    = 256,
		                                  .preferred_backend = ALP_INFERENCE_BACKEND_AUTO };
	alp_model_select_result_t r       = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_CPU);
}

ZTEST(alp_model_select, test_no_fit_no_cpu_returns_no_fit)
{
	static const uint8_t b0[4]     = { 1 };
	alp_model_t          m         = { 0 };
	m.n_targets                    = 1;
	m.targets[0]                   = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 0, 512, b0, 4);
	const char            *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t env     = {
		.soc_ref         = "alif:ensemble:e7",
		.avail_silicon   = avail,
		.n_avail_silicon = 1,
		.arena_sram_kib  = 256,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_NO_FIT);
}

ZTEST(alp_model_select, test_no_matching_backend_no_cpu_returns_no_backend)
{
	/* package only has drpai for a different SoC; device is alif, no cpu blob. */
	static const uint8_t b0[4]     = { 1 };
	alp_model_t          m         = { 0 };
	m.n_targets                    = 1;
	m.targets[0]                   = T("drpai", "renesas:rzv2n:n44", "drpai_dir", 0, 0, b0, 4);
	const char            *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t env     = {
		.soc_ref         = "alif:ensemble:e7",
		.avail_silicon   = avail,
		.n_avail_silicon = 1,
		.arena_sram_kib  = 0,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_NO_BACKEND);
}

ZTEST(alp_model_select, test_explicit_backend_absent_returns_not_found)
{
	static const uint8_t b0[4]     = { 1 };
	alp_model_t          m         = { 0 };
	m.n_targets                    = 1;
	m.targets[0]                   = T("cpu", "*", "tflite", 0, 0, b0, 4);
	const char            *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t env     = {
		.soc_ref         = "alif:ensemble:e7",
		.avail_silicon   = avail,
		.n_avail_silicon = 1,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_DEEPX_DXM1, &r),
	              ALP_ERR_NOT_FOUND);
}

ZTEST(alp_model_select, test_discrete_deepx_available_via_avail_silicon)
{
	/* V2M: host renesas + on-module deepx:dx:m1 in avail_silicon. */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m            = { 0 };
	m.n_targets                       = 2;
	m.targets[0]                      = T("deepx_dxm1", "deepx:dx:m1", "dxnn", 0, 0, b0, 4);
	m.targets[1]                      = T("cpu", "*", "tflite", 0, 0, b1, 4);
	const char               *avail[] = { "renesas:rzv2n:n44", "deepx:dx:m1" };
	alp_model_select_env_t    env     = { .soc_ref           = "renesas:rzv2n:n44",
		                                  .avail_silicon     = avail,
		                                  .n_avail_silicon   = 2,
		                                  .preferred_backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1 };
	alp_model_select_result_t r       = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_DEEPX_DXM1);
}

ZTEST(alp_model_select, test_null_args_return_inval)
{
	alp_model_t               m   = { 0 };
	alp_model_select_env_t    env = { .soc_ref = "alif:ensemble:e7" };
	alp_model_select_result_t r   = { 0 };
	zassert_equal(alp_model_select(NULL, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_INVAL);
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r),
	              ALP_ERR_INVAL); /* 0 targets */
}

ZTEST(alp_model_select, test_explicit_npu_no_fit_does_not_silently_use_cpu)
{
	/* An explicit ETHOS_U request that is available but does not fit must
     * surface NO_FIT -- it must NOT silently fall back to the CPU blob
     * (spec: an explicit backend forces a specific NPU). */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m = { 0 };
	m.n_targets            = 2;
	m.targets[0]           = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 0, 512, b0, 4);
	m.targets[1]           = T("cpu", "*", "tflite", 0, 0, b1, 4);
	const char               *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t    env     = { .soc_ref         = "alif:ensemble:e7",
		                                  .avail_silicon   = avail,
		                                  .n_avail_silicon = 1,
		                                  .arena_sram_kib  = 256 };
	alp_model_select_result_t r       = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_ETHOS_U, &r), ALP_ERR_NO_FIT);
}

ZTEST(alp_model_select, test_preferred_backend_breaks_tie_between_two_fitting_npus)
{
	/* Two NPUs both available + fitting (budget 0 = skip): the SoM
     * preferred_backend decides; flipping it flips the winner. */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 }, b2[4] = { 3 };
	alp_model_t          m = { 0 };
	m.n_targets            = 3;
	m.targets[0]           = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 0, 0, b0, 4);
	m.targets[1]           = T("deepx_dxm1", "deepx:dx:m1", "dxnn", 0, 0, b1, 4);
	m.targets[2]           = T("cpu", "*", "tflite", 0, 0, b2, 4);
	const char *avail[]    = { "deepx:dx:m1" };

	alp_model_select_env_t    env_d = { .soc_ref           = "alif:ensemble:e7",
		                                .avail_silicon     = avail,
		                                .n_avail_silicon   = 1,
		                                .arena_sram_kib    = 0,
		                                .preferred_backend = ALP_INFERENCE_BACKEND_DEEPX_DXM1 };
	alp_model_select_result_t r     = { 0 };
	zassert_equal(alp_model_select(&m, &env_d, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_DEEPX_DXM1);

	alp_model_select_env_t env_e = env_d;
	env_e.preferred_backend      = ALP_INFERENCE_BACKEND_ETHOS_U;
	zassert_equal(alp_model_select(&m, &env_e, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_ETHOS_U);
}

ZTEST(alp_model_select, test_null_env_and_out_return_inval)
{
	static const uint8_t b0[4]    = { 1 };
	alp_model_t          m        = { 0 };
	m.n_targets                   = 1;
	m.targets[0]                  = T("cpu", "*", "tflite", 0, 0, b0, 4);
	alp_model_select_env_t    env = { .soc_ref = "alif:ensemble:e7" };
	alp_model_select_result_t r   = { 0 };
	zassert_equal(alp_model_select(&m, NULL, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_INVAL);
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, NULL), ALP_ERR_INVAL);
}

ZTEST(alp_model_select, test_m_class_env_does_not_offer_drpai_falls_to_cpu)
{
	/* V2N M33 (Zephyr) env: the DRP-AI3 engine is A55/Linux-side only
     * (issue #58), so the loader passes soc_ref=NULL and an empty
     * avail_silicon.  The drpai target must NOT be offered even though
     * its silicon_ref names the very SoC the M33 sits on; AUTO falls
     * back to the CPU blob (TFLM runs fine on the M33). */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m     = { 0 };
	m.n_targets                = 2;
	m.targets[0]               = T("drpai", "renesas:rzv2n:n44", "drpai_dir", 0, 0, b0, 4);
	m.targets[1]               = T("cpu", "*", "tflite", 0, 0, b1, 4);
	alp_model_select_env_t env = {
		.soc_ref         = NULL, /* no on-SoC NPU engine driveable from this core */
		.avail_silicon   = NULL,
		.n_avail_silicon = 0,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_CPU);
	zassert_equal(r.target_index, 1u);
}

ZTEST(alp_model_select, test_m_class_env_drpai_only_returns_no_backend)
{
	/* Same M33 env, but the package carries ONLY the drpai blob: nothing
     * is runnable on this core-class -> NO_BACKEND (both for AUTO and for
     * an explicit DRPAI request -- the engine exists in the package but
     * is not available on this build, which is 'nothing available', not
     * NOT_FOUND / NO_FIT). */
	static const uint8_t b0[4] = { 1 };
	alp_model_t          m     = { 0 };
	m.n_targets                = 1;
	m.targets[0]               = T("drpai", "renesas:rzv2n:n44", "drpai_dir", 0, 0, b0, 4);
	alp_model_select_env_t env = {
		.soc_ref         = NULL,
		.avail_silicon   = NULL,
		.n_avail_silicon = 0,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_NO_BACKEND);
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_DRPAI, &r), ALP_ERR_NO_BACKEND);
}

ZTEST(alp_model_select, test_a55_env_offers_drpai_via_avail_silicon)
{
	/* A55/Yocto env with the MERA engine compiled in
     * (ALP_SDK_USE_DRPAI_V2N): the loader lists renesas:rzv2n:n44 in
     * avail_silicon -- soc_ref stays NULL, proving availability flows
     * from the engine list alone. */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m         = { 0 };
	m.n_targets                    = 2;
	m.targets[0]                   = T("drpai", "renesas:rzv2n:n44", "drpai_dir", 0, 0, b0, 4);
	m.targets[1]                   = T("cpu", "*", "tflite", 0, 0, b1, 4);
	const char            *avail[] = { "renesas:rzv2n:n44" };
	alp_model_select_env_t env     = {
		.soc_ref         = NULL,
		.avail_silicon   = avail,
		.n_avail_silicon = 1,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_DRPAI);
	zassert_equal(r.format, ALP_INFERENCE_MODEL_DRPAI);
}

ZTEST(alp_model_select, test_oversized_cpu_target_rejected)
{
	/* The CPU catch-all passes the same arena gate as every NPU target:
     * a CPU blob whose req_sram_kib exceeds the device budget must NOT be
     * selected (it would return ALP_OK and only fail far downstream at
     * arena allocation).  With nothing else in the package the verdict
     * is NO_FIT -- a target existed but was oversized. */
	static const uint8_t b0[4]     = { 1 };
	alp_model_t          m         = { 0 };
	m.n_targets                    = 1;
	m.targets[0]                   = T("cpu", "*", "tflite", 0, 512, b0, 4);
	const char            *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t env     = {
		.soc_ref         = "alif:ensemble:e7",
		.avail_silicon   = avail,
		.n_avail_silicon = 1,
		.arena_sram_kib  = 256,
	};
	alp_model_select_result_t r = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_NO_FIT);
	/* An explicit CPU request for the same oversized blob is NO_FIT too. */
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_CPU, &r), ALP_ERR_NO_FIT);
}

ZTEST(alp_model_select, test_cpu_fallback_skipped_when_cpu_oversized)
{
	/* AUTO with an oversized NPU target AND an oversized CPU target:
     * the CPU fallback must not fire either -- NO_FIT overall. */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m = { 0 };
	m.n_targets            = 2;
	m.targets[0]           = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 0, 512, b0, 4);
	m.targets[1]           = T("cpu", "*", "tflite", 0, 400, b1, 4);
	const char               *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t    env     = { .soc_ref         = "alif:ensemble:e7",
		                                  .avail_silicon   = avail,
		                                  .n_avail_silicon = 1,
		                                  .arena_sram_kib  = 256 };
	alp_model_select_result_t r       = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_AUTO, &r), ALP_ERR_NO_FIT);
}

ZTEST(alp_model_select, test_explicit_cpu_request)
{
	/* Explicit CPU: picks the cpu blob directly; with no cpu blob the
     * deliberate asymmetry is NO_BACKEND (not NOT_FOUND, which is reserved
     * for an absent explicitly-requested NPU). */
	static const uint8_t b0[4] = { 1 }, b1[4] = { 2 };
	alp_model_t          m = { 0 };
	m.n_targets            = 2;
	m.targets[0]           = T("ethos_u", "alif:ensemble:e7", "vela_tflite", 0, 0, b0, 4);
	m.targets[1]           = T("cpu", "*", "tflite", 0, 0, b1, 4);
	const char               *avail[] = { "alif:ensemble:e7" };
	alp_model_select_env_t    env     = { .soc_ref         = "alif:ensemble:e7",
		                                  .avail_silicon   = avail,
		                                  .n_avail_silicon = 1 };
	alp_model_select_result_t r       = { 0 };
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_CPU, &r), ALP_OK);
	zassert_equal(r.backend, ALP_INFERENCE_BACKEND_CPU);

	m.n_targets = 1; /* drop the cpu blob -> only ethos_u remains */
	zassert_equal(alp_model_select(&m, &env, ALP_INFERENCE_BACKEND_CPU, &r), ALP_ERR_NO_BACKEND);
}
