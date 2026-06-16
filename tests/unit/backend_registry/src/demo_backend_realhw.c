/* SPDX-License-Identifier: Apache-2.0 */

#include <alp/cap_instance.h>
#include "demo_class.h"

static int demo_realhw_open(demo_handle_t *h, uint32_t instance_id)
{
	(void)h;
	(void)instance_id;
	return 0;
}

static int demo_realhw_read(demo_handle_t *h, uint32_t *out)
{
	(void)h;
	*out = 0xCAFEu;
	return 0;
}

static const demo_ops_t demo_realhw_ops = {
	.open = demo_realhw_open,
	.read = demo_realhw_read,
};

static int demo_realhw_probe(uint32_t instance_id, uint32_t *caps)
{
	/* Instance 0 has DMA, instance 1+ does not (refine downward). */
	if (instance_id == 0u) {
		*caps |= (uint32_t)ALP_INSTANCE_CAP_DMA;
	} else {
		*caps &= ~(uint32_t)ALP_INSTANCE_CAP_DMA;
	}
	return 0;
}

ALP_BACKEND_REGISTER(demo,
                     realhw,
                     {
                         .silicon_ref = "alif:ensemble:e7",
                         .vendor      = "alif",
                         .base_caps   = (uint32_t)ALP_INSTANCE_CAP_HW_OVERSAMPLE,
                         .priority    = 100,
                         .ops         = &demo_realhw_ops,
                         .probe       = demo_realhw_probe,
                     });
