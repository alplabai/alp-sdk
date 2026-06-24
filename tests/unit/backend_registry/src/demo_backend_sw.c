/* SPDX-License-Identifier: Apache-2.0 */

#include "demo_class.h"

static int demo_sw_open(demo_handle_t *h, uint32_t instance_id)
{
	(void)h;
	(void)instance_id;
	return 0;
}

static int demo_sw_read(demo_handle_t *h, uint32_t *out)
{
	(void)h;
	*out = 0x50F7u;
	return 0;
}

static const demo_ops_t demo_sw_ops = {
	.open = demo_sw_open,
	.read = demo_sw_read,
};

ALP_BACKEND_REGISTER(demo,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &demo_sw_ops,
                         .probe       = NULL,
                     });
