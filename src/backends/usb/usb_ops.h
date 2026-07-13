/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_usb dispatcher and per-backend
 * implementations.  Covers both device and host surfaces -- they
 * share the underlying USB controller and live in one class
 * registry.  NOT a public header.
 */

#ifndef ALP_BACKENDS_USB_OPS_H
#define ALP_BACKENDS_USB_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/usb.h>

typedef struct alp_usb_ops alp_usb_ops_t;

typedef struct alp_usb_dev_state {
	alp_usb_device_config_t cfg;
	void                   *be_data;
	const alp_usb_ops_t    *ops;
} alp_usb_dev_state_t;

typedef struct alp_usb_host_state {
	void                *be_data;
	const alp_usb_ops_t *ops;
} alp_usb_host_state_t;

struct alp_usb_ops {
	/* Device-side ops */
	alp_status_t (*dev_open)(const alp_usb_device_config_t *cfg,
	                         alp_usb_dev_state_t           *state,
	                         alp_capabilities_t            *caps_out);
	alp_status_t (*dev_enable)(alp_usb_dev_state_t *state);
	alp_status_t (*dev_disable)(alp_usb_dev_state_t *state);
	alp_status_t (*dev_write)(alp_usb_dev_state_t *state,
	                          const uint8_t       *data,
	                          size_t               len,
	                          uint32_t             timeout_ms);
	alp_status_t (*dev_read)(alp_usb_dev_state_t *state,
	                         uint8_t             *data,
	                         size_t               len,
	                         size_t              *out_len,
	                         uint32_t             timeout_ms);
	void (*dev_close)(alp_usb_dev_state_t *state);

	/* Host-side ops */
	alp_status_t (*host_open)(alp_usb_host_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*host_enable)(alp_usb_host_state_t *state);
	alp_status_t (*host_disable)(alp_usb_host_state_t *state);
	void (*host_close)(alp_usb_host_state_t *state);
};

struct alp_usb_dev {
	alp_usb_dev_state_t  state;
	const alp_backend_t *backend;
	alp_capabilities_t   cached_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

struct alp_usb_host {
	alp_usb_host_state_t state;
	const alp_backend_t *backend;
	alp_capabilities_t   cached_caps;
	/* lifecycle/active_ops -- see struct alp_usb_dev above. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_USB_OPS_H */
