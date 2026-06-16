/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_camera dispatcher and per-backend
 * implementations.  NOT a public header -- customer code never
 * sees this struct.  Layout may change between SDK versions.
 *
 * Slice 5 ships only the zephyr_stub backend (every op returns
 * ALP_ERR_NOT_IMPLEMENTED); real Alif/V2N camera backends land
 * per the tracking issues on the stub source file.  No vendor
 * extensions exist for camera, so the first-member-aliasing
 * pattern the ADC vtable uses is not required here.
 */

#ifndef ALP_BACKENDS_CAMERA_OPS_H
#define ALP_BACKENDS_CAMERA_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/camera.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_camera_ops alp_camera_ops_t;

/** Backend-owned per-handle state. */
typedef struct alp_camera_backend_state {
	void                   *be_data;
	const alp_camera_ops_t *ops;
} alp_camera_backend_state_t;

/** Vtable each camera backend implements. */
struct alp_camera_ops {
	alp_status_t (*open)(const alp_camera_config_t *cfg, alp_camera_backend_state_t *state,
	                     alp_capabilities_t *caps_out);
	alp_status_t (*start)(alp_camera_backend_state_t *state);
	alp_status_t (*stop)(alp_camera_backend_state_t *state);
	alp_status_t (*capture)(alp_camera_backend_state_t *state, alp_camera_frame_t *out,
	                        uint32_t timeout_ms);
	alp_status_t (*release)(alp_camera_backend_state_t *state, alp_camera_frame_t *frame);
	alp_status_t (*configure_isp)(alp_camera_backend_state_t    *state,
	                              const alp_camera_isp_config_t *isp);
	void (*close)(alp_camera_backend_state_t *state);
};

/**
 * Handle struct layout.  Opaque to customers via the public
 * `typedef struct alp_camera alp_camera_t;` forward declaration in
 * <alp/camera.h>.  Defined here so the dispatcher
 * (src/camera_dispatch.c) and any future per-backend .c files can
 * access the fields without duplicating the layout.
 */
struct alp_camera {
	alp_camera_backend_state_t state;
	const alp_backend_t       *backend;
	alp_capabilities_t         cached_caps;
	bool                       in_use;
};

#endif /* ALP_BACKENDS_CAMERA_OPS_H */
