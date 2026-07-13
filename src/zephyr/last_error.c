/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Thread-local last-error storage for `alp_*_open` failure
 * diagnosis.  The public read accessor is `alp_last_error`; the
 * internal write accessor lives in handles.h so per-peripheral
 * source files can stamp a precise reason before returning NULL.
 */

#include <zephyr/kernel.h>

#include "alp/peripheral.h"
#include "alp_z_last_error.h"
#include "handles.h"

/* Per-thread error code.  Zephyr's __thread keyword maps to its
 * thread-local-storage facility (CONFIG_THREAD_LOCAL_STORAGE).  When
 * TLS is unavailable, fall back to a single global — concurrent
 * open() calls on multiple threads CAN then clobber each other's
 * last-error slot, which contradicts the public alp_last_error()
 * contract (<alp/peripheral.h>) on any build with more than one
 * thread.
 *
 * A single-thread build (CONFIG_MULTITHREADING=n, rare in Zephyr) has
 * no concurrent caller by construction, so the plain global there is
 * correct, not a downgrade — same reasoning as the bare-metal fallback
 * in src/common/alp_internal.h's ALP_LAST_ERROR_TLS.  A *threaded*
 * build without CONFIG_THREAD_LOCAL_STORAGE is the real gap: an app
 * that calls alp_last_error() from more than one thread MUST set
 * CONFIG_THREAD_LOCAL_STORAGE=y (the convention already used across
 * this repo's threaded example/test prj.conf files) or it silently
 * gets the process-wide global instead of the documented per-thread
 * slot.  A build-time diagnostic here (rather than this comment) was
 * tried and reverted: CONFIG_THREAD_LOCAL_STORAGE depends on
 * CONFIG_ARCH_HAS_THREAD_LOCAL_STORAGE && CONFIG_TOOLCHAIN_SUPPORTS_-
 * THREAD_LOCAL_STORAGE, and at least one first-party target
 * (native_sim, ZEPHYR_TOOLCHAIN_VARIANT=host) doesn't currently
 * satisfy the toolchain half regardless of what prj.conf requests --
 * so a loud diagnostic here would fire, with -Werror, on every
 * multithreaded Zephyr build in this SDK's own CI, not just the ones
 * that forgot to opt in.  That toolchain-support gap is tracked
 * separately from #627 (this file's job is correctness when TLS DOES
 * apply, which it does on every real E1M/E1M-X target). */
#if defined(CONFIG_THREAD_LOCAL_STORAGE)
static __thread alp_status_t z_last_err;
#else
static alp_status_t z_last_err;
#endif

void alp_z_set_last_error(alp_status_t s)
{
	z_last_err = s;
}

void alp_z_clear_last_error(void)
{
	z_last_err = ALP_OK;
}

alp_status_t alp_last_error(void)
{
	return z_last_err;
}
