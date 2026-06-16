/* SPDX-License-Identifier: Apache-2.0
 * OS-agnostic .alpmodel loader: parse -> select -> delegate to the linked
 * alp_inference_open().  Gated on CONFIG_ALP_SDK_MODEL_READER (uses the
 * zcbor reader). */
#include <alp/inference.h>
#include <alp/model.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "../backends/inference/alp_model_select.h"

/* TLS last-error setters (same ones the dispatchers use). */
#if defined(__ZEPHYR__)
extern void alp_z_set_last_error(alp_status_t s);
#define SET_ERR(s) alp_z_set_last_error(s)
#else
extern void alp_internal_set_last_error(alp_status_t s);
#define SET_ERR(s) alp_internal_set_last_error(s)
#endif

#if defined(CONFIG_ALP_SDK_MODEL_READER)

/* Device facts available to *every* SoM build via soc_caps + Kconfig.
 * The host SoC ref is always available; on-module discrete accelerators
 * are added when their backend is compiled in. */
static const char *_avail_silicon[] = {
	ALP_SOC_REF_STR,
/* The on-module DEEPX DX-M1 (V2N-M1) becomes selectable when its backend
     * is compiled in -- via the Zephyr Kconfig or the Yocto CMake option. */
#if defined(CONFIG_ALP_SDK_INFERENCE_BACKEND_DEEPX_DXM1) || defined(ALP_SDK_USE_DEEPX_DXM1)
	"deepx:dx:m1",
#endif
};

alp_inference_t *alp_inference_open_alpmodel(const alp_model_open_opts_t *opts)
{
	if (opts == NULL) {
		SET_ERR(ALP_ERR_INVAL);
		return NULL;
	}
	if (opts->data == NULL) {
		/* path-based load (opts->path) is not implemented yet; surface a
         * diagnosable NOSUPPORT rather than an INVAL that looks like a
         * malformed opts struct. */
		SET_ERR(ALP_ERR_NOSUPPORT);
		return NULL;
	}
	if (opts->size == 0u) {
		SET_ERR(ALP_ERR_INVAL);
		return NULL;
	}
	alp_model_t  mdl;
	alp_status_t rc = alp_model_parse((const uint8_t *)opts->data, opts->size, &mdl);
	if (rc != ALP_OK) {
		SET_ERR(rc); /* INVAL / VERSION */
		return NULL;
	}

	alp_model_select_env_t env = {
		.soc_ref           = ALP_SOC_REF_STR,
		.avail_silicon     = _avail_silicon,
		.n_avail_silicon   = sizeof(_avail_silicon) / sizeof(_avail_silicon[0]),
		.arena_sram_kib    = (uint32_t)ALP_SOC_NPU_ARENA_SRAM_KIB,
		.preferred_backend = ALP_INFERENCE_BACKEND_AUTO, /* board define wiring is a follow-up */
	};
	alp_model_select_result_t sel;
	rc = alp_model_select(&mdl, &env, opts->backend, &sel);
	if (rc != ALP_OK) {
		SET_ERR(rc);
		return NULL;
	}

	const alp_model_target_t *t   = &mdl.targets[sel.target_index];
	alp_inference_config_t    cfg = {
		   .model_data  = t->blob,
		   .model_size  = t->blob_len,
		   .format      = sel.format,
		   .backend     = sel.backend,
		   .arena_bytes = opts->arena_bytes ? opts->arena_bytes : sel.arena_bytes,
		   .arena       = opts->arena,
	};
	return alp_inference_open(&cfg); /* relays its own last_error on failure */
}

#else /* reader not compiled in */

alp_inference_t *alp_inference_open_alpmodel(const alp_model_open_opts_t *opts)
{
	(void)opts;
	SET_ERR(ALP_ERR_NOSUPPORT);
	return NULL;
}

#endif
