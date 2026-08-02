/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between alp_can dispatcher and per-backend
 * implementations.  NOT a public header.
 *
 * @par Issue #756 -- worker-thread callback self-close
 * The Yocto SocketCAN backend (src/backends/can/yocto_drv.c) runs a
 * per-handle RX reader thread that invokes filter callbacks directly.
 * A callback that calls alp_can_close() on its OWN handle is running
 * ON that RX thread -- the dispatcher's alp_can_close() must not join
 * that thread from inside itself (guaranteed EDEADLK).  `shutdown`/
 * `destroy` below (both optional; NULL for every backend with no
 * worker thread) split what used to be a single `close()` into the
 * same two-step protocol GHSA-xhm8-7f87-93q5 established for RPC (see
 * src/backends/rpc/rpc_ops.h's file comment for the full rationale):
 * `shutdown()` detects self-vs-external and reports which; only a
 * DONE result lets the dispatcher destroy synchronously.  A backend
 * with `shutdown` == NULL (zephyr_drv.c, sw_fallback.c, testing_drv.c
 * -- none of them has a worker thread that can invoke a callback
 * outside the calling app's own thread) keeps its existing
 * `close()`-only behaviour unchanged: the dispatcher treats a missing
 * `shutdown` as an unconditional DONE and calls `close()` (or
 * `destroy()`, if that is the one provided) immediately.
 *
 * @par Drain-before-shutdown ordering (dev-review follow-up)
 * src/can_dispatch.c's alp_can_close() drains active_ops to 0 BEFORE
 * ever calling shutdown() -- restoring the #629 invariant (CAS ->
 * drain -> teardown) every other class's close() still follows.
 * shutdown() is where a backend snapshots "does a worker thread exist,
 * and is it me" (yocto_drv.c's d->rx_running); calling it before the
 * drain let that snapshot race a concurrent, already-counted
 * alp_can_add_filter() that had not yet reached the point where it
 * actually spawns the RX thread -- shutdown() would see no thread,
 * report DONE, and the dispatcher would destroy the handle out from
 * under a thread that was still being born.  See that function's own
 * comment for the full timeline.
 */

#ifndef ALP_BACKENDS_CAN_OPS_H
#define ALP_BACKENDS_CAN_OPS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/can.h>

typedef struct alp_can_ops alp_can_ops_t;

typedef struct alp_can_backend_state {
	void                *dev; /* opaque backend device pointer
                                          * (const struct device * on Zephyr;
                                          * kept void* so the portable handle
                                          * does not pull in <zephyr/device.h>) */
	uint32_t             bus_id;
	void                *be_data; /* per-handle backend sidecar */
	const alp_can_ops_t *ops;
	/* Dispatcher-owned back-pointer to the enclosing struct alp_can,
	 * stamped by alp_can_open() before calling ops->open() -- issue
	 * #756, mirrors alp_rpc_backend_state_t::owner (rpc_ops.h).  A
	 * backend whose shutdown() returns ALP_CAN_SHUTDOWN_DEFERRED caches
	 * this (see y_can_data_t::owner in yocto_drv.c) so its RX thread's
	 * epilogue can call alp_can_close_finalize(owner) exactly once. */
	void *owner;
} alp_can_backend_state_t;

/**
 * @brief Result of @ref alp_can_ops::shutdown.  See this header's file
 *        comment for the full two-step close protocol.
 */
typedef enum {
	/** External close, or a backend with no worker thread: every
	 *  current AND future callback/blocking wait on this handle has
	 *  already terminated/been quiesced, and any worker thread has
	 *  already been joined.  active_ops is already drained (the
	 *  dispatcher drains BEFORE calling shutdown() -- see this header's
	 *  file comment); the dispatcher proceeds straight to destroy(). */
	ALP_CAN_SHUTDOWN_DONE = 0,
	/** Self-close: the caller of alp_can_close() -- and therefore of
	 *  this shutdown() call -- IS the backend's own RX/worker thread (a
	 *  filter callback closing its own handle).  The backend has NOT
	 *  joined that thread (would be a guaranteed self-join deadlock).
	 *  active_ops is already drained; the dispatcher returns immediately
	 *  without destroying anything -- the backend calls
	 *  @ref alp_can_close_finalize() exactly once, later, from that same
	 *  thread, once its read loop has unwound. */
	ALP_CAN_SHUTDOWN_DEFERRED = 1,
} alp_can_shutdown_result_t;

struct alp_can_ops {
	alp_status_t (*open)(const alp_can_config_t  *cfg,
	                     alp_can_backend_state_t *state,
	                     alp_capabilities_t      *caps_out);
	alp_status_t (*start)(alp_can_backend_state_t *state);
	alp_status_t (*stop)(alp_can_backend_state_t *state);
	alp_status_t (*send)(alp_can_backend_state_t *state,
	                     const alp_can_frame_t   *frame,
	                     uint32_t                 timeout_ms);
	alp_status_t (*add_filter)(alp_can_backend_state_t *state,
	                           const alp_can_filter_t  *filter,
	                           alp_can_rx_cb_t          cb,
	                           void                    *user,
	                           int32_t                 *filter_id_out);
	alp_status_t (*remove_filter)(alp_can_backend_state_t *state, int32_t filter_id);
	void (*close)(alp_can_backend_state_t *state);

	/** Optional (issue #756).  NULL for every backend with no worker
	 *  thread of its own (dispatcher then just runs the unconditional
	 *  drain -> close() sequence, matching the pre-#756 behaviour).
	 *  When present, called AFTER active_ops has already drained to 0
	 *  (see this header's file comment on why the drain must happen
	 *  first): detects self-vs-external close and reports which (see
	 *  alp_can_shutdown_result_t).  May block only on a bounded join of
	 *  its own worker thread (external path). */
	alp_can_shutdown_result_t (*shutdown)(alp_can_backend_state_t *state);
	/** Paired with shutdown() != NULL.  Called instead of close() once
	 *  shutdown() has returned -- either synchronously (DONE path) or
	 *  later via alp_can_close_finalize() (DEFERRED path).  active_ops
	 *  is already drained by the time either runs.  Must NOT block. */
	void (*destroy)(alp_can_backend_state_t *state);
};

/**
 * @brief Complete a deferred (self-close) teardown -- see this
 *        header's file comment and @ref alp_can_shutdown_result_t.
 *        Called EXACTLY ONCE, by a backend's own RX/worker thread,
 *        after that thread's shutdown() returned
 *        ALP_CAN_SHUTDOWN_DEFERRED and the callback that triggered the
 *        self-close has returned.
 *
 * @param[in] owner  The alp_can_backend_state_t::owner value cached by
 *                    the backend at open() time (opaque to the
 *                    backend).  NULL is a silent no-op.
 */
void alp_can_close_finalize(void *owner);

/*
 * Portable handle layout.  Holds the caller's config snapshot + a
 * started flag so the dispatcher can short-circuit double-start/stop
 * without entering the backend and so close() can consult the flag
 * to decide whether to issue can_stop on shutdown.  Zephyr-specific
 * RX callback storage lives in a sidecar inside
 * src/backends/can/zephyr_drv.c so non-Zephyr backends never touch
 * a Zephyr type.
 */
struct alp_can {
	alp_can_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;
	alp_can_config_t        cfg; /* snapshot of caller's config */
	bool                    started;
	/* lifecycle/active_ops drive the generic open/op/close guard in
	 * src/common/alp_slot_claim.h (alp_handle_op_enter/leave/
	 * begin_close, issue #629) -- placed (with in_use) after cfg/
	 * started so moving in_use to the last member (required for the
	 * atomic-claim zeroing in src/can_dispatch.c: memset up to
	 * offsetof(..., in_use)) still resets cfg/started to zero on
	 * every fresh claim, matching the pre-fix full-struct memset. */
	uint8_t  lifecycle;
	uint32_t active_ops;
	bool     in_use;
};

#endif /* ALP_BACKENDS_CAN_OPS_H */
