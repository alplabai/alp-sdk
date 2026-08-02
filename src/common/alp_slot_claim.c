/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sleep-poll close for handle pools (issue #629, and issue #1114 for
 * alp_handle_begin_close() specifically).
 *
 * alp_handle_begin_close_blocking() below is a sleep-poll drain,
 * generalised from rpc_dispatch.c's _rpc_begin_close()/_rpc_drain() to
 * the shared open/op/close guard, for a handle that counts an op
 * taking a caller `timeout_ms` (a real link-layer/broker/transfer
 * round-trip): busy-spinning a closer thread for the whole timeout
 * would starve lower-priority threads on a single core, exactly the
 * deadlock class rpc_dispatch.c's _rpc_drain() doc comment
 * (GHSA-xhm8-7f87-93q5) already spells out.
 *
 * alp_handle_begin_close() (also below, issue #1114) used to be a
 * busy-spin inline in alp_slot_claim.h, defended on the assumption
 * that every op it drains is short so the spin could only ever run
 * "a handful of instructions". That defence only accounted for op
 * *duration*; it did not account for *priority*. A higher-priority
 * closing thread that never blocks (a bare spin) never lets the
 * scheduler run a strictly lower-priority op thread, no matter how
 * few instructions that op thread needs to reach
 * alp_handle_op_leave() -- so the spin is unsafe on a single-core
 * preemptive-priority scheduler regardless of how short the op is.
 * It is now a thin wrapper over the same sleep-poll drain
 * alp_handle_begin_close_blocking() uses.
 *
 * Both are defined out-of-line here (not inline in alp_slot_claim.h)
 * because the sleep primitive needs an OS header (k_sleep / nanosleep)
 * that header deliberately does not pull in -- see its file comment:
 * it stays header-only/OS-clean (compiler atomics only) so every OS
 * backend's dispatcher TUs can include it with no extra link
 * dependency. This TU is the one place that trades that portability
 * for an actual sleep, and is linked in unconditionally alongside the
 * header (see src/common/CMakeLists.txt and zephyr/CMakeLists.txt) so
 * every dispatcher that includes alp_slot_claim.h can call either.
 */

#include "alp_slot_claim.h"

/* Portable "sleep a tick, don't spin" primitive -- copied from
 * rpc_dispatch.c's identical primitive (see that file's line-58
 * comment for the full rationale): __ZEPHYR__ is set by the Zephyr
 * toolchain itself, before any of our headers, so it is the same
 * portable OS gate src/common/alp_model_loader.c already uses to
 * avoid unconditionally pulling in <zephyr/kernel.h> from a TU that
 * also compiles into the plain-CMake baremetal/yocto trees. */
#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>
#else
#include <time.h>
#endif

static void _sleep_tick(void)
{
#if defined(__ZEPHYR__)
	k_sleep(K_TICKS(1));
#else
	struct timespec ts = { .tv_sec = 0, .tv_nsec = 1000000L }; /* 1 ms */
	nanosleep(&ts, NULL);
#endif
}

void alp_handle_drain_blocking(uint32_t *active_ops)
{
	while (__atomic_load_n(active_ops, __ATOMIC_ACQUIRE) != 0u) {
		_sleep_tick();
	}
}

bool alp_handle_begin_close_blocking(uint8_t *lifecycle, uint32_t *active_ops)
{
	if (!alp_lifecycle_cas(lifecycle, ALP_HANDLE_LC_OPEN, ALP_HANDLE_LC_CLOSING)) {
		return false;
	}
	alp_handle_drain_blocking(active_ops);
	return true;
}

bool alp_handle_begin_close(uint8_t *lifecycle, uint32_t *active_ops)
{
	/* issue #1114: this used to CAS then busy-spin inline in the header.
	 * The spin's defence ("every op here is short, so it drains in a
	 * handful of instructions") only holds for op *duration* -- it says
	 * nothing about *priority*.  On a single-core preemptive-priority
	 * scheduler a higher-priority closing thread that never blocks (a
	 * bare spin) never lets the scheduler run a lower-priority op
	 * thread, however few instructions that thread needs, so the spin
	 * never ends.  Delegating to the same sleep-poll drain
	 * alp_handle_begin_close_blocking() already uses for genuinely
	 * blocking ops fixes that: _sleep_tick() actually removes this
	 * thread from the ready queue, so the scheduler is free to run a
	 * strictly lower-priority thread -- something neither a spin nor a
	 * same-priority k_yield() can do. Short ops still close fast in
	 * practice: the drain's first check usually already sees
	 * active_ops back at 0. */
	return alp_handle_begin_close_blocking(lifecycle, active_ops);
}

/* See alp_slot_claim.h's "Reentrant self-close guard (issue #756)"
 * block comment for the full contract these two functions implement. */

bool alp_handle_begin_close_selfaware(uint8_t                 *lifecycle,
                                      uint32_t                *active_ops,
                                      const bool              *cb_active,
                                      const uintptr_t         *cb_thread,
                                      alp_handle_close_mode_t *mode,
                                      bool                    *close_pending)
{
	if (!alp_lifecycle_cas(lifecycle, ALP_HANDLE_LC_OPEN, ALP_HANDLE_LC_CLOSING)) {
		return false;
	}

	/* Acquire pairs with alp_handle_cb_enter()'s release store: seeing
	 * cb_active==true here guarantees *cb_thread is the value that
	 * same store published, not a torn/stale read. */
	if (__atomic_load_n(cb_active, __ATOMIC_ACQUIRE) && *cb_thread == alp_thread_token_self()) {
		/* Self-close: this thread is currently inside the callback-
		 * invoking op that led here.  Draining active_ops now would
		 * deadlock -- this thread's own count cannot reach zero until
		 * IT returns from that op.  Defer: the op's own wrapper
		 * finishes the close (see alp_handle_take_deferred_close()'s
		 * doc comment in alp_slot_claim.h) once it leaves. */
		__atomic_store_n(close_pending, true, __ATOMIC_RELEASE);
		*mode = ALP_HANDLE_CLOSE_DEFERRED;
		return true;
	}

	*mode = ALP_HANDLE_CLOSE_NOW;
	alp_handle_drain_blocking(active_ops);
	return true;
}
