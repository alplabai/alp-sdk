/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * RPC class dispatcher.  Owns the public <alp/rpc.h> surface
 * (framed RPC over OpenAMP / RPMsg) on top of the backend registry
 * mechanism shipped in Slice 0 (PR #17).
 *
 * Per design spec Section 4: ONE class registry covers the single
 * alp_rpc_channel_t surface.  The dispatcher copies the customer's
 * alp_rpc_config_t into the handle's backend state before
 * delegating to the backend's open(); every subsequent op walks
 * state->ops.  Subscription tables and the per-channel sync-call
 * slot live entirely behind state->be_data inside the backend.
 *
 * Slice 4c ships no vendor extensions for RPC: the Zephyr
 * ipc_service-backed backend covers every E1M-conformant SoM with
 * an OpenAMP / RPMsg-capable Zephyr build; the SW fallback covers
 * native_sim and trimmed images.  No second registry tier is
 * needed.
 *
 * @par Close-protocol redesign (GHSA-xhm8-7f87-93q5)
 * Two prior patch rounds each introduced a NEW deadlock here (see
 * src/backends/rpc/rpc_ops.h's file comment for the blow-by-blow).
 * This file now implements the authoritative three-step protocol:
 * a single atomic CAS elects one closer (_rpc_begin_close()), the
 * winner calls the backend's ops->shutdown() (DONE or DEFERRED --
 * see rpc_ops.h), and DONE alone proceeds to drain the channel's
 * active-op count to 0 (_rpc_drain(), sleep-poll, never spin) before
 * calling ops->destroy() and releasing the pool slot.  DEFERRED
 * returns immediately; the backend's own rx/worker thread completes
 * the identical drain -> destroy -> release sequence later, exactly
 * once, via alp_rpc_close_finalize() -- safe there precisely because
 * nothing else depends on that thread to make progress at that
 * point.  See _rpc_finalize() below, which is the one place that
 * sequence is implemented, shared by both call sites.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/rpc.h>
#include <alp/soc_caps.h>

#include "alp_slot_claim.h"
#include "backends/rpc/rpc_ops.h"

ALP_BACKEND_DEFINE_CLASS(rpc);
/* Pull the rpc registry section into a static-archive link (#368). */
ALP_BACKEND_ANCHOR(rpc);

#include "alp_z_last_error.h"

/* Portable "sleep a tick, don't spin" primitive for _rpc_drain() below
 * -- the RPC dispatcher compiles into both the Zephyr build and the
 * Yocto plain-CMake build (see zephyr/CMakeLists.txt +
 * src/yocto/CMakeLists.txt), so it can't unconditionally pull in
 * <zephyr/kernel.h> the way a Zephyr-only TU would; __ZEPHYR__ (set
 * by the Zephyr toolchain itself, before any of our headers) is the
 * same portable OS gate src/common/alp_model_loader.c already uses
 * for exactly this reason. */
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <time.h>
#endif

#ifndef CONFIG_ALP_SDK_MAX_RPC_HANDLES
#define CONFIG_ALP_SDK_MAX_RPC_HANDLES 2
#endif

static struct alp_rpc_channel _rpc_pool[CONFIG_ALP_SDK_MAX_RPC_HANDLES];

static struct alp_rpc_channel *_alloc_rpc(void)
{
	for (size_t i = 0; i < (size_t)CONFIG_ALP_SDK_MAX_RPC_HANDLES; ++i) {
		/* Atomic claim (GHSA-xhm8-7f87-93q5 / issue #629 pattern): CAS
         * FREE -> OPEN on `in_use` so two concurrent alp_rpc_open()
         * calls can never both win the same slot.  Only the winner may
         * touch the slot's other fields -- in_use is the struct's last
         * member, so zero everything before it. */
		if (alp_slot_try_claim(&_rpc_pool[i].in_use)) {
			memset(&_rpc_pool[i], 0, offsetof(struct alp_rpc_channel, in_use));
			return &_rpc_pool[i];
		}
	}
	return NULL;
}

static void _free_rpc(struct alp_rpc_channel *h)
{
	alp_slot_release(&h->in_use);
}

/* ================================================================== */
/* Combined lifecycle+refcount word (GHSA-xhm8-7f87-93q5)              */
/* ================================================================== */
/*
 * See struct alp_rpc_channel's doc comment in rpc_ops.h for the bit
 * layout.  Every access below is a single read-modify-write (or load)
 * on `chan_word` -- no separate lifecycle-then-refcount steps, so
 * there is no window between "observe OPEN" and "count myself" for a
 * close+reopen to slip through and hand a freshly-reopened channel's
 * slot to an op that thinks it is still touching the OLD channel.
 */

/**
 * @brief Enter a subscribe/unsubscribe/send/call op, gated on OPEN.
 *
 * Single CAS: load the word; if its lifecycle bits aren't OPEN, fail
 * without touching anything; otherwise CAS word -> word+1 (bumps only
 * the count bits -- the lifecycle bits are untouched by a +1 as long
 * as the count never overflows into them, which a
 * CONFIG_ALP_SDK_MAX_RPC_HANDLES-sized pool of concurrent callers
 * never approaches).  ACQUIRE on success synchronizes-with the
 * RELEASE store alp_rpc_open() makes when it publishes
 * ALP_RPC_CHAN_LC_OPEN, so a successful op_enter is guaranteed to see
 * every write open() made (including the backend's own state->be_data
 * publish).
 *
 * @return true if the op may proceed (counted); false if the channel
 *         is closed/closing -- the caller must bail with NOT_READY
 *         without touching backend state.
 */
static bool _rpc_op_enter(struct alp_rpc_channel *ch)
{
	uint32_t word = __atomic_load_n(&ch->chan_word, __ATOMIC_RELAXED);
	for (;;) {
		if ((word & ALP_RPC_CHAN_LC_MASK) != ALP_RPC_CHAN_LC_OPEN) {
			return false;
		}
		uint32_t next = word + 1u;
		if (__atomic_compare_exchange_n(
		        &ch->chan_word, &word, next, true, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
			return true;
		}
		/* CAS failure refreshed `word` with the current value; retry. */
	}
}

/**
 * @brief Leave an op entered via _rpc_op_enter().
 *
 * RELEASE fetch-sub so _rpc_drain()'s ACQUIRE load is guaranteed to
 * observe every write this op made to backend state before it leaves.
 */
static void _rpc_op_leave(struct alp_rpc_channel *ch)
{
	__atomic_fetch_sub(&ch->chan_word, 1u, __ATOMIC_RELEASE);
}

/**
 * @brief Single-owner election: CAS OPEN -> CLOSING, preserving the
 *        count bits untouched.
 *
 * ACQ_REL: the ACQUIRE half lets the winner safely observe every
 * op that already incremented the count (so it knows exactly what
 * _rpc_drain() below is waiting to see reach 0); the RELEASE half
 * publishes the CLOSING transition to every future _rpc_op_enter().
 *
 * @return true if this caller won the OPEN -> CLOSING transition and
 *         must now run ops->shutdown(); false if the channel was
 *         already CLOSING or UNOPENED (a racing closer, or a stale/
 *         reused handle) -- caller's alp_rpc_close() is then a no-op,
 *         matching the documented double-close contract in
 *         include/alp/rpc.h.
 */
static bool _rpc_begin_close(struct alp_rpc_channel *ch)
{
	uint32_t word = __atomic_load_n(&ch->chan_word, __ATOMIC_RELAXED);
	for (;;) {
		if ((word & ALP_RPC_CHAN_LC_MASK) != ALP_RPC_CHAN_LC_OPEN) {
			return false;
		}
		uint32_t next = (word & ALP_RPC_CHAN_CNT_MASK) | ALP_RPC_CHAN_LC_CLOSING;
		if (__atomic_compare_exchange_n(
		        &ch->chan_word, &word, next, true, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
			return true;
		}
	}
}

/**
 * @brief Wait for every op counted before the CLOSING transition to
 *        leave (count bits reach 0).
 *
 * MUST sleep, never busy-spin: a busy spin on a single-core target
 * livelocks forever whenever the closer thread runs at equal-or-
 * higher priority than the thread still holding the count (the exact
 * trap `k_yield()` alone doesn't fix either -- k_yield only cedes to
 * threads of the SAME priority -- just fixed the same way in the ulog
 * engine lock).  A short, bounded sleep between checks actually makes
 * the closer non-ready for that tick, which is what lets the
 * scheduler run a lower-priority holder at all.
 *
 * By the time ops->shutdown() has returned (DONE: from the external-
 * close path; or this function is being run from
 * alp_rpc_close_finalize() on the backend's own rx/worker thread after
 * a DEFERRED self-close), every backend mechanism that could still be
 * blocking an in-flight op has already been cancelled/woken -- see
 * rpc_ops.h's alp_rpc_shutdown_result_t doc comment -- so this drains
 * in a small, bounded number of iterations, not an open-ended wait.
 *
 * Deliberately NOT alp_handle_begin_close() from alp_slot_claim.h:
 * that helper's own doc comment states its precondition explicitly --
 * every op it drains must be a short, synchronous backend call.
 * alp_rpc_call() is not (it can block up to `timeout_ms`, including
 * UINT32_MAX == forever, unblocked only by the backend's own
 * shutdown()) -- so this dispatcher owns its own drain, here, rather
 * than reusing that helper outside its documented precondition.
 */
static void _rpc_drain(struct alp_rpc_channel *ch)
{
	for (;;) {
		uint32_t word = __atomic_load_n(&ch->chan_word, __ATOMIC_ACQUIRE);
		if ((word & ALP_RPC_CHAN_CNT_MASK) == 0u) {
			return;
		}
#if defined(__ZEPHYR__)
		k_sleep(K_TICKS(1));
#else
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
		nanosleep(&ts, NULL);
#endif
	}
}

/**
 * @brief Shared drain -> destroy -> UNOPENED -> slot-release sequence.
 *
 * The ONE place both close-protocol outcomes converge:
 *   - alp_rpc_close(), synchronously, when ops->shutdown() returned
 *     ALP_RPC_SHUTDOWN_DONE; or
 *   - alp_rpc_close_finalize(), later, on the backend's own rx/worker
 *     thread, exactly once, when ops->shutdown() returned
 *     ALP_RPC_SHUTDOWN_DEFERRED.
 *
 * Both call sites reach this only after this channel's single-owner
 * CAS (_rpc_begin_close()) has already run and ops->shutdown() has
 * already returned, so no OTHER close of this same channel can still
 * be in flight.
 */
static void _rpc_finalize(struct alp_rpc_channel *ch)
{
	_rpc_drain(ch);

	if (ch->state.ops != NULL && ch->state.ops->destroy != NULL) {
		ch->state.ops->destroy(&ch->state);
	}

	/* Only now -- after the drain has observed count == 0 and destroy()
     * has freed every backend resource -- does this slot become
     * recyclable.  Order matters: a concurrent alp_rpc_open() gated on
     * `in_use` (see _alloc_rpc()) must never be able to claim this
     * slot while chan_word still reads CLOSING with a live count, or
     * while the backend's state.be_data still points at memory
     * destroy() hasn't freed yet. */
	__atomic_store_n(&ch->chan_word, ALP_RPC_CHAN_LC_UNOPENED, __ATOMIC_RELEASE);
	_free_rpc(ch);
}

/**
 * @brief Complete a deferred (self-close) teardown -- see this
 *        function's declaration in src/backends/rpc/rpc_ops.h.
 */
void alp_rpc_close_finalize(void *owner)
{
	struct alp_rpc_channel *ch = (struct alp_rpc_channel *)owner;
	if (ch == NULL) {
		return;
	}
	_rpc_finalize(ch);
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

alp_rpc_channel_t *alp_rpc_open(const alp_rpc_config_t *cfg)
{
	alp_z_clear_last_error();
	if (cfg == NULL || cfg->name == NULL || cfg->name[0] == '\0') {
		alp_z_set_last_error(ALP_ERR_INVAL);
		return NULL;
	}
	const alp_backend_t *be = alp_backend_select("rpc", ALP_SOC_REF_STR);
	if (be == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_PRESENT_ON_THIS_SOC);
		return NULL;
	}
	const alp_rpc_ops_t *ops = (const alp_rpc_ops_t *)be->ops;
	if (ops == NULL || ops->open == NULL) {
		alp_z_set_last_error(ALP_ERR_NOT_IMPLEMENTED);
		return NULL;
	}
	struct alp_rpc_channel *h = _alloc_rpc();
	if (h == NULL) {
		alp_z_set_last_error(ALP_ERR_NOMEM);
		return NULL;
	}
	h->backend   = be;
	h->state.ops = ops;
	h->state.cfg = *cfg;
	/* Stamp the dispatcher-owned back-pointer BEFORE calling into the
     * backend's open() -- a backend that later self-closes caches this
     * (see rpc_ops.h's alp_rpc_backend_state_t::owner doc comment) so
     * its rx/worker thread can call alp_rpc_close_finalize(owner)
     * exactly once from its own epilogue. */
	h->state.owner          = h;
	alp_capabilities_t caps = { .flags = be->base_caps };
	alp_status_t       rc   = ops->open(cfg, &h->state, &caps);
	if (rc != ALP_OK) {
		_free_rpc(h);
		alp_z_set_last_error(rc);
		return NULL;
	}
	h->cached_caps = caps;
	/* Only now, after a successful backend open(), does this handle
     * become visible to alp_rpc_close()/subscribe/send/call's
     * _rpc_op_enter() gate -- see struct alp_rpc_channel's doc comment
     * in rpc_ops.h.  RELEASE publishes everything ops->open() wrote
     * (including its own state.be_data) to every _rpc_op_enter() whose
     * CAS observes this OPEN transition (ACQUIRE) -- see
     * _rpc_op_enter()'s own doc comment. */
	__atomic_store_n(&h->chan_word, ALP_RPC_CHAN_LC_OPEN, __ATOMIC_RELEASE);
	return h;
}

void alp_rpc_close(alp_rpc_channel_t *ch)
{
	if (ch == NULL) return;

	/* Step 1 -- single-owner election.  A racing closer (an external
     * alp_rpc_close() racing this same channel's own self-close
     * callback -- the scenario this advisory is about, or any other
     * racing closer, or a stale handle whose slot has already been
     * recycled) loses this CAS and returns immediately without ever
     * touching `state`.
     *
     * Residual, NOT defended -- same as the pre-redesign contract:
     * `alp_rpc_channel_t` is a raw pointer with no generation the
     * caller carries across calls.  A caller that calls
     * alp_rpc_close() a SECOND time on a handle that has already
     * completed a full close (and whose slot has since been recycled
     * by an unrelated alp_rpc_open()) will legitimately win this same
     * CAS against the NEW occupant and tear IT down -- a use-after-
     * close by the caller, the same class of bug as any other use of
     * a stale handle, and out of scope for a fix that keeps
     * alp_rpc_channel_t opaque -- see alp_rpc_close()'s doc comment in
     * include/alp/rpc.h. */
	if (!_rpc_begin_close(ch)) return;

	/* Step 2 -- the backend's own shutdown/self-close detection. */
	alp_rpc_shutdown_result_t result = ALP_RPC_SHUTDOWN_DONE;
	if (ch->state.ops != NULL && ch->state.ops->shutdown != NULL) {
		result = ch->state.ops->shutdown(&ch->state);
	}

	if (result == ALP_RPC_SHUTDOWN_DEFERRED) {
		/* Step 3b -- self-close: the caller of alp_rpc_close() (and
         * therefore of shutdown() above) IS the backend's own rx/
         * worker thread.  Return IMMEDIATELY: no drain, no destroy.
         * The slot stays in_use=true, chan_word CLOSING -- structurally
         * unrecyclable (_alloc_rpc() claims on `in_use`, which is
         * still held; every _rpc_op_enter() fails on CLOSING; a
         * concurrent external close loses _rpc_begin_close()'s CAS and
         * no-ops; a concurrent open() skips this slot entirely).
         * Later, the backend's rx-epilogue calls
         * alp_rpc_close_finalize(owner) exactly once, which runs the
         * identical drain -> destroy -> UNOPENED -> release sequence
         * from that thread -- safe there because, by that point,
         * nothing else depends on the rx/worker thread to make
         * progress. */
		return;
	}

	/* Step 3a -- external close (or a backend with no self-close
     * possibility, like sw_fallback / the bare-metal stub): finish the
     * teardown synchronously, right here, on the caller's own thread. */
	_rpc_finalize(ch);
}

/* ================================================================== */
/* Subscriptions                                                       */
/* ================================================================== */

alp_status_t
alp_rpc_subscribe(alp_rpc_channel_t *ch, const char *method, alp_rpc_method_cb_t cb, void *user)
{
	/* Gate through _rpc_op_enter(), not a plain `in_use` bool read --
     * see struct alp_rpc_channel's doc comment (GHSA-xhm8-7f87-93q5). */
	if (ch == NULL || !_rpc_op_enter(ch)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->subscribe == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->subscribe(&ch->state, method, cb, user);
	}
	_rpc_op_leave(ch);
	return rc;
}

alp_status_t alp_rpc_unsubscribe(alp_rpc_channel_t *ch, const char *method)
{
	if (ch == NULL || !_rpc_op_enter(ch)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->unsubscribe == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->unsubscribe(&ch->state, method);
	}
	_rpc_op_leave(ch);
	return rc;
}

/* ================================================================== */
/* Send + call                                                         */
/* ================================================================== */

alp_status_t
alp_rpc_send(alp_rpc_channel_t *ch, const char *method, const void *payload, size_t len)
{
	if (ch == NULL || !_rpc_op_enter(ch)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (payload == NULL && len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->send == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->send(&ch->state, method, payload, len);
	}
	_rpc_op_leave(ch);
	return rc;
}

alp_status_t alp_rpc_call(alp_rpc_channel_t *ch,
                          const char        *method,
                          const void        *req,
                          size_t             req_len,
                          void              *resp,
                          size_t            *resp_len,
                          uint32_t           timeout_ms)
{
	if (ch == NULL || !_rpc_op_enter(ch)) {
		return ALP_ERR_NOT_READY;
	}
	alp_status_t rc;
	if (method == NULL || method[0] == '\0') {
		rc = ALP_ERR_INVAL;
	} else if (req == NULL && req_len > 0) {
		rc = ALP_ERR_INVAL;
	} else if (resp != NULL && resp_len == NULL) {
		rc = ALP_ERR_INVAL;
	} else if (ch->state.ops == NULL || ch->state.ops->call == NULL) {
		rc = ALP_ERR_NOT_IMPLEMENTED;
	} else {
		rc = ch->state.ops->call(&ch->state, method, req, req_len, resp, resp_len, timeout_ms);
	}
	_rpc_op_leave(ch);
	return rc;
}

/* ================================================================== */
/* Capability getter                                                   */
/* ================================================================== */

const alp_capabilities_t *alp_rpc_capabilities(const alp_rpc_channel_t *ch)
{
	return (ch != NULL) ? &ch->cached_caps : NULL;
}
