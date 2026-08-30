/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Power stub backend.  Wildcard ("*") registration at priority 0:
 * picks up every silicon_ref the build targets so apps that
 * #include <alp/power.h> link cleanly on every supported SoC.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/613 (Yocto/Linux
 *      power backend; wildcard stub returns ALP_ERR_NOSUPPORT until it lands).
 *
 * Behaviour differs from the Camera / Display / GPU2D stubs:
 * stub_open returns ALP_OK so the dispatcher hands the caller a
 * real handle, and reports *wake_caps_out = 0 (this backend can arm
 * zero real wake sources).  The dispatcher (src/power_dispatch.c)
 * enforces the reported-capability + error contract centrally against
 * that value -- ANY non-empty bitmap is refused with ALP_ERR_NOSUPPORT
 * at alp_power_configure_wake_source() time, before this backend's own
 * op even runs, so setup code that ignores wake_caps finds out at
 * configuration time rather than after a sleep that could never wake
 * (#1812).  request_sleep is the final backstop for a caller that
 * skips configure_wake_source entirely (e.g. a wake_after_ms-only
 * request): it always returns ALP_ERR_NOSUPPORT too, since this
 * backend can never actually sleep.
 *
 * Real backends already exist and out-rank this wildcard at higher
 * priority on the silicon_refs they claim: src/backends/power/
 * zephyr_pm_policy.c (Zephyr pm_policy_* + per-SoC pm_state tables,
 * "*" at priority 100, gated by ALP_SDK_POWER_PM_POLICY / CONFIG_PM),
 * src/backends/ext/renesas/power.c (GD32G553 supervisor
 * CMD_POWER_MODE_SET opcode 0x28 on V2N, gated by
 * ALP_SDK_POWER_EXT_RENESAS), and src/backends/power/
 * alif_se_profile.c (Alif SE aiPM operating-point profile on the
 * separate "power_profile" class, gated by
 * ALP_SDK_POWER_PROFILE_ALIF_SE), and src/backends/power/yocto_drv.c
 * (real Linux `/sys/power/state` + `/sys/class/rtc/rtc0/wakealarm`
 * backend, "*" at priority 100, #613).  This stub only wins where none
 * of those are linked into the build, or none claims the build's
 * silicon_ref -- e.g. a non-Linux plain-CMake Yocto cross-build where
 * yocto_drv.c compiles to an empty object.
 */

#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "power_ops.h"

static alp_status_t
stub_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out, uint32_t *wake_caps_out)
{
	(void)state;
	(void)caps_out; /* no alp_instance_cap_t bits apply to power */
	/* Successful open is required so the dispatcher hands the caller
     * a handle; alp_power_open always returns a valid pointer.  The
     * real "this feature isn't implemented" surface is at
     * configure_wake_source / request_sleep below.
     *
     * wake_caps_out = 0: this backend can arm zero real wake sources.
     * The dispatcher enforces that centrally against every
     * configure_wake_source() call (#1813). */
	if (wake_caps_out != NULL) {
		*wake_caps_out = 0u;
	}
	return ALP_OK;
}

static alp_status_t stub_configure_wake_source(alp_power_backend_state_t *state,
                                               uint32_t                   wake_bitmap)
{
	/* The dispatcher already rejected any bitmap outside wake_caps
     * (0 for this backend, see stub_open) before this op runs -- only
     * ALP_POWER_WAKE_NONE can reach here, which is trivially OK. */
	(void)state;
	(void)wake_bitmap;
	return ALP_OK;
}

static alp_status_t stub_request_sleep(alp_power_backend_state_t *state,
                                       alp_power_mode_t           mode,
                                       uint32_t                   wake_after_ms,
                                       alp_power_wake_info_t     *info)
{
	(void)state;
	(void)wake_after_ms;
	if (info != NULL) {
		info->realised_mode = mode;
		info->wake_source   = 0u;
		info->slept_ms      = 0u;
	}
	/* No real PM backend on this build: report NOSUPPORT (matching the
     * <alp/power.h> portable contract), not NOT_IMPLEMENTED.  open()
     * still succeeds so setup code links/runs; the dispatcher already
     * rejects a non-empty wake bitmap before this point (see
     * stub_open), so this is the backstop for a wake_after_ms-only
     * (bitmap-free) request. */
	return ALP_ERR_NOSUPPORT;
}

static const alp_power_ops_t _ops = {
	.open                  = stub_open,
	.configure_wake_source = stub_configure_wake_source,
	.configure_retention   = NULL, /* dispatcher default: NONE ok, else NOSUPPORT */
	.request_sleep         = stub_request_sleep,
	.close                 = NULL,
};

ALP_BACKEND_ANCHOR_DEFINE(power);
ALP_BACKEND_REGISTER(power,
                     zephyr_stub,
                     {
                         .silicon_ref = "*",
                         .vendor      = "stub",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

/* ------------------------------------------------------------------ */
/* Operating-point-profile stub (class "power_profile")                 */
/*                                                                      */
/* Wildcard priority-0 registration so alp_power_profile_get/_set link  */
/* + return gracefully on every build.  No portable software source     */
/* exists for an operating-point profile, so both ops report            */
/* ALP_ERR_NOSUPPORT; real backends (the Alif SE aiPM body) register    */
/* per silicon_ref at higher priority.                                  */
/* ------------------------------------------------------------------ */

static alp_status_t stub_profile_get(alp_power_profile_id_t which, alp_power_profile_t *out)
{
	(void)which;
	(void)out; /* dispatcher already zero-filled it */
	return ALP_ERR_NOSUPPORT;
}

static alp_status_t stub_profile_set(alp_power_profile_id_t     which,
                                     const alp_power_profile_t *profile)
{
	(void)which;
	(void)profile;
	return ALP_ERR_NOSUPPORT;
}

static const alp_power_profile_ops_t _profile_ops = {
	.get = stub_profile_get,
	.set = stub_profile_set,
};

ALP_BACKEND_ANCHOR_DEFINE(power_profile);
ALP_BACKEND_REGISTER(power_profile,
                     zephyr_stub,
                     {
                         .silicon_ref = "*",
                         .vendor      = "stub",
                         .base_caps   = 0u,
                         .priority    = 0,
                         .ops         = &_profile_ops,
                         .probe       = NULL,
                     });
