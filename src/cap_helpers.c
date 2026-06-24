/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Hand-written helpers for the ALP capability API.  NOT auto-
 * generated -- this file holds runtime logic that does not depend
 * on per-SoC metadata.
 */

#include <stddef.h>
#include <alp/cap_instance.h>

bool alp_capabilities_has(const alp_capabilities_t *c, alp_instance_cap_t f)
{
	if (c == NULL) {
		return false;
	}
	return (c->flags & (uint32_t)f) != 0u;
}
