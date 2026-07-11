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
	/* lifecycle/active_ops build on the generic open/op/close primitives
     * in src/common/alp_slot_claim.h (alp_lifecycle_cas/
     * alp_handle_op_enter/leave -- issue #629's pattern, applied here
     * for GHSA-xhm8-7f87-93q5 defect 2): every op in src/rpc_dispatch.c
     * gates on `lifecycle`, not `in_use`. `in_use` is touched only by
     * the atomic pool-slot claim/release in _alloc_rpc()/_free_rpc().
     * NOTE: alp_rpc_close() does NOT call the ready-made
     * alp_handle_begin_close() helper -- that helper drains
     * `active_ops` to zero BEFORE running the caller's close body,
     * which assumes every gated op is short; alp_rpc_call() is not (it
     * can block up to its `timeout_ms`, unblocked only by the
     * backend's own close). alp_rpc_close() instead does CAS ->
     * ops->close() -> drain, so see its own doc comment for the full
     * ordering argument. Order matters: _free_rpc() (alp_slot_release)
     * runs strictly AFTER ops->close() has returned AND `active_ops`
     * has drained to 0 AND `lifecycle` has been reset to
     * ALP_HANDLE_LC_UNOPENED, so a concurrent alp_rpc_open() can never
     * recycle this slot -- and hand a DIFFERENT, freshly-opened channel
     * out through the very pointer a still-in-flight op is
     * dereferencing -- while that op or close is still running. `in_use`
     * is last (as in every other *_dispatch.c using this pattern) so a
     * fresh claim's memset(..., offsetof(..., in_use)) zeroes
     * `lifecycle`/`active_ops` back to ALP_HANDLE_LC_UNOPENED / 0
     * without clobbering the flag the CAS just flipped. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

/* ------------------------------------------------------------------ */
/* Shared frame-size helper                                            */
/* ------------------------------------------------------------------ */

/*
 * Compute the on-wire framed length for a `method_len`-byte method name
 * (plus its NUL terminator) followed by a `payload_len`-byte payload,
 * storing it in *total_out.  Returns false if the frame would not fit
 * in `cap`.
 *
 * Overflow-safe: the naive `method_len + 1 + payload_len` sum can wrap
 * size_t for a near-SIZE_MAX payload_len and slip past a `total > cap`
 * comparison, so the room left in the frame is computed by subtraction
 * only.  Both the Zephyr and Yocto frame builders route through this.
 */
static inline bool
alp_rpc_frame_size(size_t method_len, size_t payload_len, size_t cap, size_t *total_out)
{
	if (method_len >= cap) {
		return false; /* no room for the method name + its NUL */
	}
	size_t avail = cap - method_len - 1u; /* >= 0: method_len < cap */
	if (payload_len > avail) {
		return false;
	}
	*total_out = method_len + 1u + payload_len; /* <= cap, cannot wrap */
	return true;
}

#endif /* ALP_BACKENDS_RPC_OPS_H */
