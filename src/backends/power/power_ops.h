/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_power dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Slice 7 ships only the zephyr_stub backend; request_sleep returns
 * ALP_ERR_NOT_IMPLEMENTED while open / configure_wake_source succeed
 * silently so the legacy "handle is always returned, sleep is the
 * gate" contract from src/zephyr/power_zephyr.c is preserved.  Real
 * Alif/V2N power backends land per the tracking issue on the stub
 * source file.  No vendor extensions exist for power, so the
 * first-member-aliasing pattern the ADC vtable uses is not required
 * here.
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
 *  configured wake_bitmap here so backends that don't keep their own
 *  copy can read it back at request_sleep() time. */
typedef struct alp_power_backend_state {
	uint32_t               wake_bitmap;
	void                  *be_data;
	const alp_power_ops_t *ops;
} alp_power_backend_state_t;

/** Vtable each power backend implements. */
struct alp_power_ops {
	alp_status_t (*open)(alp_power_backend_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*configure_wake_source)(alp_power_backend_state_t *state, uint32_t wake_bitmap);
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
	bool                      in_use;
};

#endif /* ALP_BACKENDS_POWER_OPS_H */
