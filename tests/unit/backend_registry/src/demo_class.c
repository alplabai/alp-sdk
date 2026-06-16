/* SPDX-License-Identifier: Apache-2.0 */

#include <stddef.h>
#include "demo_class.h"
#include <alp/peripheral.h>
#include <alp/soc_caps.h>

ALP_BACKEND_DEFINE_CLASS(demo);

int demo_open(demo_handle_t *h, uint32_t instance_id)
{
	if (h == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	const alp_backend_t *be = alp_backend_select("demo", ALP_SOC_REF_STR);
	if (be == NULL) {
		return ALP_ERR_NOT_PRESENT_ON_THIS_SOC;
	}
	h->backend                  = be;
	h->caps.flags               = be->base_caps;
	h->caps.max_sample_rate     = 0u;
	h->caps.max_resolution_bits = 0u;
	h->caps.channel_count       = 1u;
	if (be->probe != NULL) {
		uint32_t refined = h->caps.flags;
		(void)be->probe(instance_id, &refined);
		h->caps.flags = refined;
	}
	const demo_ops_t *ops = (const demo_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return ops->open(h, instance_id);
}

int demo_read(demo_handle_t *h, uint32_t *out)
{
	if (h == NULL || out == NULL || h->backend == NULL) {
		return ALP_ERR_NOSUPPORT;
	}
	const demo_ops_t *ops = (const demo_ops_t *)h->backend->ops;
	if (ops == NULL || ops->read == NULL) {
		return ALP_ERR_NOT_IMPLEMENTED;
	}
	return ops->read(h, out);
}

const alp_capabilities_t *demo_capabilities(const demo_handle_t *h)
{
	return (h != NULL) ? &h->caps : NULL;
}
