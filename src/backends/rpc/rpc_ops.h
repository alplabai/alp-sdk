/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_rpc dispatcher and per-backend
 * implementations.  Single handle type (alp_rpc_channel_t) -- the
 * RPC class wraps a single OpenAMP / RPMsg channel surface; framing
 * + per-method subscription tables live entirely behind the ops
 * vtable so the dispatcher need only know about the channel-level
 * primitives (open / close / subscribe / unsubscribe / send / call).
 *
 * Per-channel backend state is stored inside the backend itself
 * (an OpenAMP endpoint pointer + a subscription table on Zephyr; a
 * NULL pointer on the SW fallback) and reached through
 * state->be_data.  The dispatcher owns the public alp_rpc_channel_t
 * pool, copies the customer's alp_rpc_config_t into state->cfg
 * before dispatching open(), and walks state->ops for every
 * subsequent op.
 *
 * NOT a public header.
 */

#ifndef ALP_BACKENDS_RPC_OPS_H
#define ALP_BACKENDS_RPC_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>

typedef struct alp_rpc_ops alp_rpc_ops_t;

/* ------------------------------------------------------------------ */
/* Backend-owned per-handle state                                      */
/* ------------------------------------------------------------------ */

typedef struct alp_rpc_backend_state {
	alp_rpc_config_t     cfg;     /* cached customer config */
	void                *be_data; /* backend-private per-channel block */
	const alp_rpc_ops_t *ops;
} alp_rpc_backend_state_t;

/* ------------------------------------------------------------------ */
/* Ops vtable                                                          */
/* ------------------------------------------------------------------ */

struct alp_rpc_ops {
	alp_status_t (*open)(const alp_rpc_config_t  *cfg,
	                     alp_rpc_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);
	alp_status_t (*subscribe)(alp_rpc_backend_state_t *state,
	                          const char              *method,
	                          alp_rpc_method_cb_t      cb,
	                          void                    *user);
	alp_status_t (*unsubscribe)(alp_rpc_backend_state_t *state, const char *method);
	alp_status_t (*send)(alp_rpc_backend_state_t *state,
	                     const char              *method,
	                     const void              *payload,
	                     size_t                   len);
	alp_status_t (*call)(alp_rpc_backend_state_t *state,
	                     const char              *method,
	                     const void              *req,
	                     size_t                   req_len,
	                     void                    *resp,
	                     size_t                  *resp_len,
	                     uint32_t                 timeout_ms);
	void (*close)(alp_rpc_backend_state_t *state);
};

/* ------------------------------------------------------------------ */
/* Public handle layout -- owned by the dispatcher pool                */
/* ------------------------------------------------------------------ */

struct alp_rpc_channel {
	alp_rpc_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	bool                    in_use;
};

#endif /* ALP_BACKENDS_RPC_OPS_H */
