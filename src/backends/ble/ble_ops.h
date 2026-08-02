/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_ble dispatcher and per-backend
 * implementations.  Covers both handle types -- alp_ble_t (radio
 * singleton) and alp_ble_conn_t (per-connection) -- which share a
 * single 'ble' class registry per the design spec Section 4
 * decision: the controller is one piece of hardware, so the ops
 * vtable carries function pointers for both handle types and the
 * dispatcher owns two separate handle pools (radio + conn) keyed
 * off the same backend.  NOT a public header.
 */

#ifndef ALP_BACKENDS_BLE_OPS_H
#define ALP_BACKENDS_BLE_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/ble.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

typedef struct alp_ble_ops alp_ble_ops_t;

typedef struct alp_ble_radio_state {
	void                *be_data;
	const alp_ble_ops_t *ops;
} alp_ble_radio_state_t;

typedef struct alp_ble_conn_state {
	struct alp_ble      *radio; /* back-ref to owning radio handle */
	void                *be_data;
	const alp_ble_ops_t *ops;
} alp_ble_conn_state_t;

struct alp_ble_ops {
	/* Radio-side ops */
	alp_status_t (*open)(alp_ble_radio_state_t *state, alp_capabilities_t *caps_out);
	alp_status_t (*advertise_start)(alp_ble_radio_state_t *state, const alp_ble_adv_config_t *cfg);
	alp_status_t (*advertise_stop)(alp_ble_radio_state_t *state);
	alp_status_t (*gatt_register_service)(alp_ble_radio_state_t       *state,
	                                      const alp_ble_service_def_t *def,
	                                      alp_ble_attr_handle_t       *handles_out);
	alp_status_t (*gatt_notify)(alp_ble_radio_state_t *radio_state,
	                            alp_ble_conn_state_t  *conn_state,
	                            alp_ble_attr_handle_t  handle,
	                            const uint8_t         *payload,
	                            size_t                 len);
	alp_status_t (*scan_start)(alp_ble_radio_state_t *state,
	                           bool                   active,
	                           alp_ble_scan_cb_t      cb,
	                           void                  *user);
	alp_status_t (*scan_stop)(alp_ble_radio_state_t *state);
	alp_status_t (*connect)(alp_ble_radio_state_t *state,
	                        const alp_ble_addr_t  *peer,
	                        uint32_t               timeout_ms,
	                        alp_ble_conn_state_t  *conn_state_out);
	void (*close)(alp_ble_radio_state_t *state);

	/* Conn-side ops */
	alp_status_t (*disconnect)(alp_ble_conn_state_t *conn_state);
	alp_status_t (*gatt_read)(alp_ble_conn_state_t *conn_state,
	                          alp_ble_attr_handle_t handle,
	                          uint8_t              *out,
	                          size_t                out_cap,
	                          size_t               *out_len,
	                          uint32_t              timeout_ms);
	alp_status_t (*gatt_write)(alp_ble_conn_state_t *conn_state,
	                           alp_ble_attr_handle_t handle,
	                           const uint8_t        *data,
	                           size_t                len,
	                           uint32_t              timeout_ms);
};

struct alp_ble {
	alp_ble_radio_state_t state;
	const alp_backend_t  *backend;
	alp_capabilities_t    cached_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim.
	 *
	 * cb_thread/cb_active/close_pending (issue #756): alp_ble_scan_start()
	 * invokes the scan callback SYNCHRONOUSLY, inline, before returning
	 * -- a callback that calls alp_ble_close() on its own radio handle
	 * used to deadlock waiting for its own still-in-flight scan_start()
	 * call to leave.  These three fields back the reentrant self-close
	 * guard in src/common/alp_slot_claim.h; see
	 * alp_ble_scan_start()/alp_ble_close() in src/ble_dispatch.c. */
	uint8_t   lifecycle;
	uint32_t  active_ops;
	uintptr_t cb_thread;
	bool      cb_active;
	bool      close_pending;
	bool      in_use;
};

struct alp_ble_conn {
	alp_ble_conn_state_t state;
	const alp_backend_t *backend;
	/* Same lifecycle/active_ops contract as struct alp_ble above; this
	 * pool's teardown is alp_ble_disconnect() (issue #629). */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_BLE_OPS_H */
