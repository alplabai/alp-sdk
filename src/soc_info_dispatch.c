/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * soc_info class dispatcher.  Owns the SoC-identity half of the
 * public <alp/hw_info.h> surface (alp_soc_info_read /
 * alp_soc_secure_fw_ping) on top of the backend registry.
 *
 * Handle-less (the TMU pattern): both entry points are stateless, so
 * there is no handle pool -- the dispatcher caches the selected
 * backend's ops vtable on first call and routes through it.
 *
 * Division of labour: the dispatcher zero-fills the out-struct and
 * stamps soc_ref from the build-time ALP_SOC_REF_STR, so "which
 * silicon is this build for" is answered on EVERY build -- including
 * native_sim and SoCs with no runtime identity source, where the
 * wildcard sw_fallback then reports ALP_ERR_NOSUPPORT for the
 * runtime fields.  Real backends (e.g. the Alif SE-service backend)
 * register per silicon_ref at higher priority and fill the runtime
 * fields over their controller transport.
 */

#include <stddef.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/hw_info.h>
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

#include "backends/soc_info/soc_info_ops.h"

ALP_BACKEND_DEFINE_CLASS(soc_info);
ALP_BACKEND_ANCHOR(soc_info);

static const alp_soc_info_ops_t *_cached_ops = NULL;

static const alp_soc_info_ops_t *_get_ops(void)
{
	if (_cached_ops != NULL) {
		return _cached_ops;
	}
	const alp_backend_t *be = alp_backend_select("soc_info", ALP_SOC_REF_STR);
	if (be == NULL) {
		return NULL;
	}
	_cached_ops = (const alp_soc_info_ops_t *)be->ops;
	return _cached_ops;
}

alp_status_t alp_soc_info_read(alp_soc_info_t *out)
{
	if (out == NULL) {
		return ALP_ERR_INVAL;
	}
	memset(out, 0, sizeof(*out));
	/* soc_ref is build-time truth -- stamp it before the backend runs
	 * so it survives every runtime-failure path. */
	strncpy(out->soc_ref, ALP_SOC_REF_STR, sizeof(out->soc_ref) - 1u);

	const alp_soc_info_ops_t *ops = _get_ops();
	if (ops == NULL || ops->read == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	return ops->read(out);
}

alp_status_t alp_soc_secure_fw_ping(void)
{
	const alp_soc_info_ops_t *ops = _get_ops();
	if (ops == NULL || ops->ping == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	return ops->ping();
}
