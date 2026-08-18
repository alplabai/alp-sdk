/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Inference NOSUPPORT stubs -- <alp/inference.h>.  Split out of the
 * former src/common/stub_backend.c monolith (issue #673); owns every
 * `alp_inference_*` symbol not provided by a vendor backend.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/inference.h"
#include "alp/peripheral.h"

/* The Yocto backend overrides these via inference_yocto.c so the
 * dispatcher can route ALP_INFERENCE_BACKEND_DEEPX_DXM1 (etc.) to a
 * real NPU adapter -- in that case ALP_VENDOR_OVERRIDES_INFERENCE
 * is set and the stub bodies below are excluded from the link. */
#if !defined(ALP_VENDOR_OVERRIDES_INFERENCE)
alp_inference_t *alp_inference_open(const alp_inference_config_t *cfg)
{
	(void)cfg;
	return NULL;
}
size_t alp_inference_num_inputs(alp_inference_t *i)
{
	(void)i;
	return 0u;
}
size_t alp_inference_num_outputs(alp_inference_t *i)
{
	(void)i;
	return 0u;
}
alp_status_t alp_inference_get_input(alp_inference_t *i, size_t idx, alp_inference_tensor_t *o)
{
	(void)i;
	(void)idx;
	if (o) *o = (alp_inference_tensor_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_inference_get_output(alp_inference_t *i, size_t idx, alp_inference_tensor_t *o)
{
	(void)i;
	(void)idx;
	if (o) *o = (alp_inference_tensor_t){ 0 };
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_inference_invoke(alp_inference_t *i)
{
	(void)i;
	return ALP_ERR_NOSUPPORT;
}
alp_status_t alp_inference_last_invoke_latency_us(alp_inference_t *i, uint64_t *out_us)
{
	(void)i;
	/* No inference backend at all in this build -- unlike the real
	 * dispatchers' NOT_READY ("no successful invoke yet"), there is no
	 * timing mechanism here to ever populate, so NOSUPPORT is the
	 * honest answer regardless of @p out_us. */
	(void)out_us;
	return ALP_ERR_NOSUPPORT;
}
void alp_inference_close(alp_inference_t *i)
{
	(void)i;
}
#endif /* !ALP_VENDOR_OVERRIDES_INFERENCE */

/* Unguarded -- inference_yocto.c (ALP_VENDOR_OVERRIDES_INFERENCE=1 on
 * Yocto) routes open/invoke/etc. to the real NPU adapter but never
 * implements alp_inference_capabilities; only the Zephyr registry
 * dispatcher (src/inference_dispatch.c, never compiled outside Zephyr)
 * does (#593). */
const alp_capabilities_t *alp_inference_capabilities(const alp_inference_t *i)
{
	(void)i;
	return NULL;
}
