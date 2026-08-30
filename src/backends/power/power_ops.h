/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_power dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Backends registered against this "power"-class vtable: zephyr_stub.c
 * (wildcard priority-0 fallback), zephyr_pm_policy.c (Zephyr
 * pm_policy_*, priority 100), and yocto_drv.c (real Linux
 * /sys/power/state + RTC wakealarm backend, priority 100, #613).
 * alif_se_profile.c implements the SEPARATE "power_profile" class
 * vtable below, not this one.  src/backends/ext/renesas/power.c is
 * NEITHER: it implements no alp_power_ops_t at all -- it's a vendor-
 * ext bypass function (alp_renesas_power_supervisor_mode_set) that
 * reads an ALREADY-OPENED alp_power_t handle's backend/state fields
 * directly (see the struct alp_power layout below), so no vendor
 * extension needs the first-member-aliasing pattern the ADC vtable
 * uses.
 */

#ifndef ALP_BACKENDS_POWER_OPS_H
#define ALP_BACKENDS_POWER_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>

typedef struct alp_power_ops alp_power_ops_t;

/** Backend-owned per-handle state.  The dispatcher mirrors the
 *  configured wake_bitmap / retain descriptor here so backends that
 *  don't keep their own copy can read them back at request_sleep()
 *  time. */
typedef struct alp_power_backend_state {
	uint32_t               wake_bitmap;
	alp_power_retain_t     retain;
	void                  *be_data;
	const alp_power_ops_t *ops;
} alp_power_backend_state_t;

/** Vtable each power backend implements.
 *
 *  @c configure_retention is OPTIONAL (NULL is a valid vtable entry,
 *  #1813) -- a backend with nothing to say about retention leaves it
 *  unset; the dispatcher then accepts only @ref ALP_POWER_RETAIN_NONE
 *  (a no-op) and returns @ref ALP_ERR_NOSUPPORT for any other level,
 *  matching the reported-capability + error contract. */
struct alp_power_ops {
	/** @p wake_caps_out is a SEPARATE channel from @p caps_out: the
	 *  latter is the cross-class alp_instance_cap_t bitmap
	 *  (cap_instance.h) every peripheral class shares, the former is
	 *  the power class's own ALP_POWER_WAKE_* arm-able-bits report
	 *  (#1813) -- the two must never alias one storage location, or
	 *  alp_capabilities_has(caps, ALP_INSTANCE_CAP_DMA) collides
	 *  bit-for-bit with ALP_POWER_WAKE_RTC. */
	alp_status_t (*open)(alp_power_backend_state_t *state,
	                     alp_capabilities_t        *caps_out,
	                     uint32_t                  *wake_caps_out);
	alp_status_t (*configure_wake_source)(alp_power_backend_state_t *state, uint32_t wake_bitmap);
	/** @p retain is the request THIS call is deciding; @c state->retain
	 *  is NOT yet updated to it -- the dispatcher mirrors @p retain
	 *  into @c state->retain only after this op returns @ref ALP_OK
	 *  (same ordering as @c state->wake_bitmap for
	 *  @c configure_wake_source, documented at its call site in
	 *  zephyr_pm_policy.c's z_configure_wake_source).  An implementer
	 *  that needs "what was previously accepted" reads @c state->retain
	 *  BEFORE using @p retain for "what's being requested now" --
	 *  reading @c state->retain expecting it to already hold @p retain
	 *  is the trap. */
	alp_status_t (*configure_retention)(alp_power_backend_state_t *state,
	                                    const alp_power_retain_t  *retain);
	alp_status_t (*request_sleep)(alp_power_backend_state_t *state,
	                              alp_power_mode_t           mode,
	                              uint32_t                   wake_after_ms,
	                              alp_power_wake_info_t     *info);
	void (*close)(alp_power_backend_state_t *state);
};

/** Vtable for the handle-less operating-point-profile surface
 *  (alp_power_profile_get / alp_power_profile_set).
 *
 *  Deliberately a SEPARATE registry class ("power_profile") from the
 *  sleep-mode class above: a silicon-specific profile backend must
 *  not displace the portable request_sleep winner (the registry picks
 *  one backend per class), and the profile surface needs no handle.
 *  The dispatcher validates `which` before dispatching. */
typedef struct alp_power_profile_ops {
	alp_status_t (*get)(alp_power_profile_id_t which, alp_power_profile_t *out);
	alp_status_t (*set)(alp_power_profile_id_t which, const alp_power_profile_t *profile);
} alp_power_profile_ops_t;

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_power alp_power_t;` forward declaration in
 * <alp/power.h>.  Defined here so the dispatcher
 * (src/power_dispatch.c) and any future per-backend .c files can
 * access the fields without duplicating the layout.
 */
struct alp_power {
	alp_power_backend_state_t state;
	const alp_backend_t      *backend;
	alp_capabilities_t        cached_caps;
	/** ALP_POWER_WAKE_* bits the active backend reported at open()
	 *  time it can actually arm; see @ref alp_power_wake_capabilities
	 *  and the vtable comment on open() above (#1813). */
	uint32_t wake_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_POWER_OPS_H */
