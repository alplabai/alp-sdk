/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * soc_info software fallback.  Wildcard ("*") registration at
 * priority 0: keeps the class section populated on every build so
 * apps that #include <alp/hw_info.h> link cleanly everywhere
 * (native_sim included).
 *
 * Best-effort by design: the DISPATCHER already stamped soc_ref from
 * the build-time ALP_SOC_REF_STR before this backend runs, so the
 * "which silicon is this build for" answer is always available.  The
 * runtime fields (secure-firmware version, part number, die revision,
 * lifecycle, serial) have no software source, so both entry points
 * report ALP_ERR_NOSUPPORT -- graceful, never a link error.
 *
 * @par Cost: negligible ROM (two stub entry points that return
 *      ALP_ERR_NOSUPPORT), RAM 0 bytes (no per-handle state).
 * @par Performance: O(1) per call; both entry points return
 *      immediately, deterministic for test assertions.
 */

#include <alp/backend.h>
#include <alp/hw_info.h>
#include <alp/peripheral.h>

#include "soc_info_ops.h"

static alp_status_t sw_read(alp_soc_info_t *out)
{
	(void)out; /* soc_ref already stamped by the dispatcher */
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t sw_ping(void)
{
	return ALP_ERR_NOSUPPORT;
}

static const alp_soc_info_ops_t _ops = {
	.read = sw_read,
	.ping = sw_ping,
};

ALP_BACKEND_REGISTER(soc_info,
                     sw_fallback,
                     {
                         .silicon_ref = "*",
                         .vendor      = "sw_fallback",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });
