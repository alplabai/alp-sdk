/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Sleep-poll close for handle pools that count a blocking op
 * (issue #629).
 *
 * alp_slot_claim.h's alp_handle_begin_close() is a documented,
 * deliberate busy-spin whose precondition is "every op it drains is a
 * short, synchronous backend call" -- fine for the controller-class
 * handles that only ever do that, but wrong for a handle that also
 * counts an op taking a caller `timeout_ms` (a real link-layer/
 * broker/transfer round-trip): busy-spinning a closer thread for the
 * whole timeout starves lower-priority threads on a single core,
 * exactly the deadlock class rpc_dispatch.c's _rpc_drain() doc
 * comment (GHSA-xhm8-7f87-93q5) already spells out.
 *
 * alp_handle_begin_close_blocking() below is that same sleep-poll
 * drain, generalised from rpc_dispatch.c's _rpc_begin_close()/
 * _rpc_drain() to the shared open/op/close guard.  It lives in its
 * own .c TU (not inline in alp_slot_claim.h) because the sleep
 * primitive needs an OS header (k_sleep / nanosleep) that header
 * deliberately does not pull in -- see its file comment: it stays
 * header-only/OS-clean (compiler atomics only) so every OS backend's
 * dispatcher TUs can include it with no extra link dependency.  This
 * TU is the one place that trades that portability for an actual
 * sleep, and is only linked in by the dispatchers that need it.
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

bool alp_handle_begin_close_blocking(uint8_t *lifecycle, uint32_t *active_ops)
{
	if (!alp_lifecycle_cas(lifecycle, ALP_HANDLE_LC_OPEN, ALP_HANDLE_LC_CLOSING)) {
		return false;
	}
	while (__atomic_load_n(active_ops, __ATOMIC_ACQUIRE) != 0u) {
		_sleep_tick();
	}
	return true;
}
