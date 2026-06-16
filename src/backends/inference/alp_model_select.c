/* SPDX-License-Identifier: Apache-2.0 */
#include "alp_model_select.h"
#include <string.h>

static alp_inference_backend_t _backend_enum(const char *s)
{
	if (strcmp(s, "cpu") == 0) {
		return ALP_INFERENCE_BACKEND_CPU;
	}
	if (strcmp(s, "ethos_u") == 0) {
		return ALP_INFERENCE_BACKEND_ETHOS_U;
	}
	if (strcmp(s, "drpai") == 0) {
		return ALP_INFERENCE_BACKEND_DRPAI;
	}
	if (strcmp(s, "deepx_dxm1") == 0) {
		return ALP_INFERENCE_BACKEND_DEEPX_DXM1;
	}
	return ALP_INFERENCE_BACKEND_AUTO; /* sentinel: unknown */
}

static alp_inference_model_format_t _fmt_enum(const char *s)
{
	if (strcmp(s, "vela_tflite") == 0) {
		return ALP_INFERENCE_MODEL_VELA;
	}
	if (strcmp(s, "drpai_dir") == 0) {
		return ALP_INFERENCE_MODEL_DRPAI;
	}
	if (strcmp(s, "dxnn") == 0) {
		return ALP_INFERENCE_MODEL_DXNN;
	}
	return ALP_INFERENCE_MODEL_TFLITE; /* "tflite" + default */
}

/* A target is available if its silicon_ref is the cpu wildcard, the host
 * SoC, or any compiled-in discrete accelerator ref. */
static bool _silicon_available(const char *ref, const alp_model_select_env_t *e)
{
	if (strcmp(ref, "*") == 0) {
		return true;
	}
	if (e->soc_ref != NULL && strcmp(ref, e->soc_ref) == 0) {
		return true;
	}
	for (size_t i = 0; i < e->n_avail_silicon; ++i) {
		if (strcmp(ref, e->avail_silicon[i]) == 0) {
			return true;
		}
	}
	return false;
}

/* SRAM gate: 0 budget = unknown -> always fits. */
static bool _fits(const alp_model_target_t *t, const alp_model_select_env_t *e)
{
	return e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib;
}

alp_status_t alp_model_select(const alp_model_t *m, const alp_model_select_env_t *env,
                              alp_inference_backend_t requested, alp_model_select_result_t *out)
{
	if (m == NULL || env == NULL || out == NULL || m->n_targets == 0u) {
		return ALP_ERR_INVAL;
	}

	int  best = -1, cpu = -1;
	bool any_backend = false;

	for (uint32_t i = 0; i < m->n_targets; ++i) {
		const alp_model_target_t *t  = &m->targets[i];
		alp_inference_backend_t   be = _backend_enum(t->backend);

		if (be == ALP_INFERENCE_BACKEND_AUTO) {
			continue; /* unknown backend string */
		}
		if (!_silicon_available(t->silicon_ref, env)) {
			continue;
		}

		if (be == ALP_INFERENCE_BACKEND_CPU) {
			cpu = (int)i;
			continue;
		}

		/* explicit-backend request: only that backend is eligible */
		if (requested != ALP_INFERENCE_BACKEND_AUTO && be != requested) {
			continue;
		}

		any_backend = true;
		if (!_fits(t, env)) {
			continue;
		}

		if (best < 0) {
			best = (int)i;
			continue;
		}
		/* tiebreak: SoM preferred_backend wins */
		alp_inference_backend_t cur = _backend_enum(m->targets[best].backend);

		if (env->preferred_backend != ALP_INFERENCE_BACKEND_AUTO && be == env->preferred_backend &&
		    cur != env->preferred_backend) {
			best = (int)i;
		}
	}

	/* explicit backend requested but no matching target at all */
	if (requested != ALP_INFERENCE_BACKEND_AUTO && requested != ALP_INFERENCE_BACKEND_CPU &&
	    best < 0 && !any_backend) {
		/* distinguish "requested NPU absent from package" from a fit failure */
		bool present = false;

		for (uint32_t i = 0; i < m->n_targets; ++i) {
			if (_backend_enum(m->targets[i].backend) == requested) {
				present = true;
				break;
			}
		}
		if (!present) {
			return ALP_ERR_NOT_FOUND;
		}
	}

	/* CPU fallback applies only to AUTO (or an explicit CPU request).  An
     * explicit NPU request that was available but did not fit must surface
     * NO_FIT -- not silently run on CPU (spec: an explicit backend forces a
     * specific NPU). */
	if (best < 0 &&
	    (requested == ALP_INFERENCE_BACKEND_AUTO || requested == ALP_INFERENCE_BACKEND_CPU)) {
		best = cpu;
	}
	if (best < 0) {
		return any_backend ? ALP_ERR_NO_FIT : ALP_ERR_NO_BACKEND;
	}

	const alp_model_target_t *t = &m->targets[best];

	out->target_index           = (uint32_t)best;
	out->backend                = _backend_enum(t->backend);
	out->format                 = _fmt_enum(t->blob_format);
	out->arena_bytes            = t->arena_bytes;
	return ALP_OK;
}
