/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_mqtt dispatcher and per-backend
 * implementations.  MQTT is a per-broker client (not a hardware
 * peripheral), so the dispatcher caches the user's broker
 * configuration in the state struct and forwards it to the backend
 * via state->cfg.  NOT a public header.
 */

#ifndef ALP_BACKENDS_MQTT_OPS_H
#define ALP_BACKENDS_MQTT_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/iot.h>
#include <alp/peripheral.h>

typedef struct alp_mqtt_ops alp_mqtt_ops_t;

typedef struct alp_mqtt_backend_state {
	alp_mqtt_config_t     cfg; /* cached customer config */
	void                 *be_data;
	const alp_mqtt_ops_t *ops;
} alp_mqtt_backend_state_t;

struct alp_mqtt_ops {
	alp_status_t (*open)(const alp_mqtt_config_t  *cfg,
	                     alp_mqtt_backend_state_t *state,
	                     alp_capabilities_t       *caps_out);
	alp_status_t (*connect)(alp_mqtt_backend_state_t *state, uint32_t timeout_ms);
	alp_status_t (*publish)(alp_mqtt_backend_state_t *state,
	                        const char               *topic,
	                        const uint8_t            *payload,
	                        size_t                    len,
	                        alp_mqtt_qos_t            qos,
	                        bool                      retain);
	alp_status_t (*subscribe)(alp_mqtt_backend_state_t *state,
	                          const char               *topic_filter,
	                          alp_mqtt_qos_t            qos,
	                          alp_mqtt_msg_cb_t         cb,
	                          void                     *user);
	alp_status_t (*loop)(alp_mqtt_backend_state_t *state, uint32_t timeout_ms);
	void (*close)(alp_mqtt_backend_state_t *state);
};

struct alp_mqtt {
	alp_mqtt_backend_state_t state;
	const alp_backend_t     *backend;
	alp_capabilities_t       cached_caps;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed before in_use so the atomic-
	 * claim zeroing in the dispatcher (memset up to
	 * offsetof(..., in_use)) resets both on every fresh claim.  EVERY
	 * op (connect/publish/subscribe/loop) is counted via active_ops --
	 * see the dispatcher's file comment.
	 *
	 * cb_thread/cb_active/close_pending (issue #756): alp_mqtt_loop()
	 * invokes the subscribed message callback SYNCHRONOUSLY, inline,
	 * before returning -- a callback that calls alp_mqtt_close() on its
	 * own handle used to deadlock waiting for its own still-in-flight
	 * loop() call to leave.  These three fields back the reentrant
	 * self-close guard in src/common/alp_slot_claim.h; see
	 * alp_mqtt_loop()/alp_mqtt_close() in src/mqtt_dispatch.c. */
	uint8_t   lifecycle;
	uint32_t  active_ops;
	uintptr_t cb_thread;
	bool      cb_active;
	bool      close_pending;
	bool      in_use;
};

#endif /* ALP_BACKENDS_MQTT_OPS_H */
