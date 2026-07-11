/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Internal ABI between the alp_rpc dispatcher and per-backend
 * implementations.  Single handle type (alp_rpc_channel_t) -- the
 * RPC class wraps a single OpenAMP / RPMsg channel surface; framing
 * + per-method subscription tables live entirely behind the ops
 * vtable so the dispatcher need only know about the channel-level
 * primitives (open / shutdown / destroy / subscribe / unsubscribe /
 * send / call).
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
 *
 * @par Close-protocol redesign (GHSA-xhm8-7f87-93q5)
 * Two prior patch rounds each introduced a NEW deadlock in the
 * dispatcher/backend close path (round 1: a callback self-close
 * self-joined its own rx thread; round 2: the dispatcher's
 * CAS -> ops->close() -> drain ordering was safe for the trivial
 * sw_fallback/stub backend but gave no way for a backend whose
 * close is invoked FROM its own rx/worker thread to avoid either
 * self-joining or leaving the dispatcher's active_ops drain spinning
 * forever waiting for an op that can only unblock via a close this
 * function has already returned from).  This header now encodes the
 * authoritative, adversarially-reviewed three-step protocol:
 *
 *   1. Owner election happens in the dispatcher (src/rpc_dispatch.c):
 *      a single atomic CAS OPEN -> CLOSING on the channel's combined
 *      lifecycle+refcount word (see struct alp_rpc_channel below).
 *      Exactly one caller ever wins; every other racing closer is a
 *      safe, immediate no-op.  No backend involvement.
 *   2. The CAS winner calls ops->shutdown(state), which returns
 *      ALP_RPC_SHUTDOWN_DONE (an external caller closed the channel --
 *      the trivial answer for sw_fallback/the bare-metal stub) or
 *      ALP_RPC_SHUTDOWN_DEFERRED (the backend detected that the
 *      caller IS its own rx/worker thread -- a subscriber callback
 *      closing its own channel).
 *   3. DONE: the dispatcher itself drains the channel's in-flight-op
 *      count to 0 (sleep-poll, never spin -- see rpc_dispatch.c) and
 *      calls ops->destroy(state), which frees every backend resource
 *      and must not block.  DEFERRED: alp_rpc_close() returns
 *      IMMEDIATELY -- no drain, no destroy.  The slot stays claimed
 *      (in_use) with lifecycle CLOSING, so it is structurally
 *      unrecyclable, until the backend's own rx-epilogue -- running
 *      on that same rx/worker thread, after the callback that
 *      triggered the self-close returns and the backend's receive
 *      loop unwinds -- calls @ref alp_rpc_close_finalize() EXACTLY
 *      ONCE, which runs the identical drain -> destroy -> UNOPENED ->
 *      slot-release sequence from that thread.  That is safe there
 *      precisely because, by that point, nothing else depends on the
 *      rx/worker thread to make progress.
 *
 * See each backend (src/backends/rpc/yocto_drv.c,
 * src/backends/rpc/zephyr_drv.c) for how `shutdown()` detects
 * self-vs-external and implements the sticky, lock-guarded `closing`
 * predicate every blocking wait (alp_rpc_call) rechecks -- replacing
 * the previous one-shot cancel, which missed a call that staged
 * itself after the one-shot cancel had already run.
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

	/* Dispatcher-owned back-pointer to the enclosing alp_rpc_channel_t
	 * (see struct alp_rpc_channel below), stamped by the dispatcher
	 * before calling ops->open().  Internal-only field on an
	 * already-internal header -- NO public ABI impact.  A backend
	 * whose ops->shutdown() returns ALP_RPC_SHUTDOWN_DEFERRED caches
	 * this (typically into its own per-channel block, alongside
	 * be_data) so its rx/worker thread's epilogue can call
	 * @ref alp_rpc_close_finalize(owner) exactly once, later, once
	 * nothing depends on that thread to make further progress. */
	void *owner;
} alp_rpc_backend_state_t;

/* ------------------------------------------------------------------ */
/* Shutdown result                                                     */
/* ------------------------------------------------------------------ */

/**
 * @brief Result of @ref alp_rpc_ops::shutdown.
 *
 * See this header's file comment for the full three-step close
 * protocol these two outcomes select between.
 */
typedef enum {
	/** External close (or a backend, like sw_fallback/the bare-metal
	 *  stub, with no self-close possibility at all): shutdown() has
	 *  already made every current AND future blocking wait on this
	 *  channel terminate promptly, and (for a backend with an rx/
	 *  worker thread) has already joined/quiesced it -- the dispatcher
	 *  proceeds straight to drain -> destroy. */
	ALP_RPC_SHUTDOWN_DONE = 0,

	/** Self-close: the caller of alp_rpc_close() -- and therefore of
	 *  this shutdown() call -- IS the backend's own rx/worker thread
	 *  (a subscriber callback closing its own channel).  shutdown()
	 *  has still made every blocking wait terminate promptly, but the
	 *  backend has NOT joined/quiesced its own rx/worker thread (it
	 *  cannot -- that would self-join/self-wait).  The dispatcher
	 *  returns from alp_rpc_close() immediately without draining or
	 *  destroying anything; the backend calls
	 *  @ref alp_rpc_close_finalize() exactly once, later, from that
	 *  same thread. */
	ALP_RPC_SHUTDOWN_DEFERRED = 1,
} alp_rpc_shutdown_result_t;

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

	/** Guarantees on return (both outcomes): every current AND future
	 *  blocking wait on this channel (e.g. a pending/late-staging
	 *  alp_rpc_call) terminates promptly via a STICKY `closing`
	 *  predicate checked under the wait's own lock -- never a re-
	 *  cancel poll.  See @ref alp_rpc_shutdown_result_t for what
	 *  differs between the two outcomes.  May block only on bounded
	 *  joins (e.g. joining/quiescing its own rx/worker thread on the
	 *  external-close path). */
	alp_rpc_shutdown_result_t (*shutdown)(alp_rpc_backend_state_t *state);

	/** Called exactly once by the dispatcher, at active-op count == 0,
	 *  strictly after shutdown() has run and every op counted before
	 *  the close won its CAS has left.  Frees every backend resource
	 *  (fds, mutexes/condvars, heap blocks, pool slots).  Must NOT
	 *  block. */
	void (*destroy)(alp_rpc_backend_state_t *state);
};

/* ------------------------------------------------------------------ */
/* Public handle layout -- owned by the dispatcher pool                */
/* ------------------------------------------------------------------ */

/* Combined lifecycle + active-op-count word (GHSA-xhm8-7f87-93q5):
 * bits[31:30] hold the lifecycle state, bits[29:0] hold the in-flight
 * op count.  Folding both into ONE atomic word (rather than a
 * separate lifecycle byte + refcount, the pre-redesign layout) turns
 * every access -- op-enter's "is it open, then count me in", op-
 * leave's decrement, begin-close's OPEN -> CLOSING transition, and
 * drain's "has the count reached 0" -- into a single read-modify-
 * write (or load) on ONE object.  That gives op-enter/begin-close a
 * single indivisible compare-exchange instead of a separate
 * increment-then-check (which had a recycle race: a checker could
 * observe OPEN, get preempted, and increment its count into a slot
 * that has since been closed AND reopened by someone else) and
 * establishes a real, portable C11 total modification order across
 * all four operations with no extra fences and no reliance on
 * sequential consistency.
 *
 * See src/rpc_dispatch.c for the encode/decode helpers
 * (_rpc_op_enter/_rpc_op_leave/_rpc_begin_close/_rpc_drain) and the
 * three-step close protocol built on top of them. */
#define ALP_RPC_CHAN_LC_SHIFT 30u
#define ALP_RPC_CHAN_LC_MASK  (0x3u << ALP_RPC_CHAN_LC_SHIFT)
#define ALP_RPC_CHAN_CNT_MASK ((1u << ALP_RPC_CHAN_LC_SHIFT) - 1u)

#define ALP_RPC_CHAN_LC_UNOPENED (0u << ALP_RPC_CHAN_LC_SHIFT)
#define ALP_RPC_CHAN_LC_OPEN     (1u << ALP_RPC_CHAN_LC_SHIFT)
#define ALP_RPC_CHAN_LC_CLOSING  (2u << ALP_RPC_CHAN_LC_SHIFT)

struct alp_rpc_channel {
	alp_rpc_backend_state_t state;
	const alp_backend_t    *backend;
	alp_capabilities_t      cached_caps;

	/* `chan_word` (see the ALP_RPC_CHAN_* macros above) replaces the
	 * pre-redesign `lifecycle` byte + `active_ops` u32 pair -- every
	 * op in src/rpc_dispatch.c gates through it via _rpc_op_enter()/
	 * _rpc_op_leave(); alp_rpc_close() gates the single-owner election
	 * through _rpc_begin_close(). `in_use` is touched only by the
	 * atomic pool-slot claim/release in _alloc_rpc()/_free_rpc() and
	 * stays last (as in every other *_dispatch.c using this general
	 * claim/release pattern) so a fresh claim's
	 * memset(..., offsetof(..., in_use)) zeroes `chan_word` back to
	 * ALP_RPC_CHAN_LC_UNOPENED / count 0 without clobbering the flag
	 * the claim itself just flipped. */
	uint32_t chan_word;
	bool     in_use;
};

/* ------------------------------------------------------------------ */
/* Internal dispatcher entry point for the DEFERRED close path         */
/* ------------------------------------------------------------------ */

/**
 * @brief Complete a deferred (self-close) teardown.
 *
 * Called EXACTLY ONCE, by a backend's rx/worker thread, after that
 * thread's ops->shutdown() returned @ref ALP_RPC_SHUTDOWN_DEFERRED and
 * the callback that triggered the self-close has returned -- i.e.
 * once nothing else depends on that thread to make progress.  Runs
 * the same drain -> destroy -> UNOPENED -> slot-release sequence
 * alp_rpc_close() runs synchronously for the ALP_RPC_SHUTDOWN_DONE
 * (external-close) case.
 *
 * @param[in] owner  The @c alp_rpc_backend_state_t::owner value cached
 *                    by the backend at open() time (opaque to the
 *                    backend; it is this same channel's
 *                    alp_rpc_channel_t*).  NULL is a silent no-op.
 */
void alp_rpc_close_finalize(void *owner);

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
