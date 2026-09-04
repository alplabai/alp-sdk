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

/* Every format string the .alpmodel writer (scripts/alp_model/manifest.py)
 * can emit must have an explicit case here.  This used to default every
 * unrecognised string to ALP_INFERENCE_MODEL_TFLITE: a typo'd or newly added
 * format silently ran the TFLite parser and reported ALP_OK, which is
 * undebuggable from a customer's side -- nothing on the wire says "decoded
 * as the wrong format".  ExecuTorch had been silently mis-decoding as
 * TFLite since ALP_INFERENCE_MODEL_EXECUTORCH was added to the enum, because
 * no case for it was ever added here.  Report failure instead so the caller
 * can surface a real error. */
static bool _fmt_enum(const char *s, alp_inference_model_format_t *out)
{
	if (strcmp(s, "tflite") == 0) {
		*out = ALP_INFERENCE_MODEL_TFLITE;
		return true;
	}
	if (strcmp(s, "vela_tflite") == 0) {
		*out = ALP_INFERENCE_MODEL_VELA;
		return true;
	}
	if (strcmp(s, "drpai_dir") == 0) {
		*out = ALP_INFERENCE_MODEL_DRPAI;
		return true;
	}
	if (strcmp(s, "dxnn") == 0) {
		*out = ALP_INFERENCE_MODEL_DXNN;
		return true;
	}
	if (strcmp(s, "executorch") == 0) {
		/* scripts/alp_model/adapters/executorch.py is the host-side
		 * writer that emits this string (issue #1260). */
		*out = ALP_INFERENCE_MODEL_EXECUTORCH;
		return true;
	}
	if (strcmp(s, "onnx") == 0) {
		*out = ALP_INFERENCE_MODEL_ONNX;
		return true;
	}
	return false;
}

/* A target is available if its silicon_ref is the cpu wildcard or one of
 * the env's ENGINE-backed refs (soc_ref when the on-SoC NPU engine is
 * compiled in, plus every avail_silicon entry).  The env owner encodes
 * the core-class truth -- see alp_model_select_env_t in the header. */
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

/* SRAM gate: 0 budget = unknown -> always fits, permissively.  This stays a
 * pass rather than a fail-closed reject (issue #1731): ALP_SOC_NPU_ARENA_SRAM_KIB
 * is 0 on all nine real SoCs today because the figure is an integration
 * decision (how much on-die SRAM this SKU's firmware reserves for the NPU
 * tensor arena vs. everything else), not a datasheet constant any vendor
 * publishes -- rejecting every selection on every real SoC until that
 * per-SKU decision is made would break inference on hardware that works
 * today, over a fact nobody can currently supply.  What changes is that the
 * pass is no longer SILENT: alp_model_select() now flags every such
 * unverified pass on its result (arena_fit_unverified) instead of returning
 * ALP_OK indistinguishably from a budget that was actually checked. */
static bool _fits(const alp_model_target_t *t, const alp_model_select_env_t *e)
{
	return e->arena_sram_kib == 0u || t->req_sram_kib <= e->arena_sram_kib;
}

alp_status_t alp_model_select(const alp_model_t            *m,
                              const alp_model_select_env_t *env,
                              alp_inference_backend_t       requested,
                              alp_model_select_result_t    *out)
{
	if (m == NULL || env == NULL || out == NULL || m->n_targets == 0u) {
		return ALP_ERR_INVAL;
	}

	int  best = -1, cpu = -1;
	bool any_backend = false, cpu_no_fit = false;

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
			/* The CPU candidate must pass the same arena gate every NPU
			 * target passes: an oversized CPU blob selected here would
			 * return ALP_OK and only fail far downstream at arena
			 * allocation (issue #245).  Remember that a CPU target
			 * existed but did not fit so the caller gets NO_FIT, not
			 * NO_BACKEND. */
			if (_fits(t, env)) {
				cpu = (int)i;
			} else {
				cpu_no_fit = true;
			}
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
		/* NO_FIT when at least one target was available but oversized
		 * (NPU or CPU); NO_BACKEND when nothing was available at all. */
		return (any_backend || cpu_no_fit) ? ALP_ERR_NO_FIT : ALP_ERR_NO_BACKEND;
	}

	const alp_model_target_t *t = &m->targets[best];

	if (!_fmt_enum(t->blob_format, &out->format)) {
		/* The selection loop above only checks backend + silicon + SRAM
		 * fit; a chosen target's blob_format is decoded here for the
		 * first time.  An unrecognised string means the manifest names
		 * a format this dispatcher cannot parse -- surface that as a
		 * real error rather than let out->format hold a stale/default
		 * value from a prior call. */
		return ALP_ERR_INVAL;
	}

	out->target_index = (uint32_t)best;
	out->backend      = _backend_enum(t->backend);
	out->arena_bytes  = t->arena_bytes;
	/* The winning target only ever passed the SRAM gate unconditionally
	 * (not by comparison) when the device published no arena budget at
	 * all -- see _fits() and the field's own doc comment. */
	out->arena_fit_unverified = (env->arena_sram_kib == 0u);
	return ALP_OK;
}
