/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared core of the split stub backend for the Alp SDK: the one
 * canonical last-error slot and the delay primitives.  Split out of
 * the former src/common/stub_backend.c monolith (issue #673) --
 * every sibling `stub_<class>.c` in this directory owns the
 * NOSUPPORT body for one public API class; this TU owns the two
 * pieces of shared state/behaviour every backend needs regardless of
 * class: `alp_last_error()` and `alp_delay_us`/`alp_delay_ms`.
 *
 * Backends that do real work (currently `src/zephyr/`) override
 * selectively via per-class Kconfig and CMake gating; backends
 * without a working impl yet (`src/baremetal/`, `src/yocto/`)
 * compile the full `src/common/stub/*.c` source set so the
 * resulting library is link-complete.
 */

#include <stddef.h>
#include <stdint.h>

#include "alp/peripheral.h"

#include "../alp_internal.h"
#include "stub_internal.h"

/* ------------------------------------------------------------------ */
/* alp_last_error — one canonical last-error slot, thread-local on a   */
/* hosted Linux target (ALP_LAST_ERROR_TLS, see alp_internal.h).       */
/*                                                                      */
/* This is the single storage every non-Zephyr layer reads/writes:     */
/* cross-TU writers (incl. the vendor/<som> peripheral wrappers under  */
/* ALP_VENDOR_OVERRIDES_PERIPHERAL) go through alp_internal_set_last_-  */
/* error; local writers in the sibling stub_<class>.c files write      */
/* z_last_error directly for brevity, via the `extern` declaration in  */
/* stub_internal.h.  Defined unconditionally -- no vendor build owns a  */
/* separate static or a duplicate alp_last_error reader anymore.       */
/* ------------------------------------------------------------------ */

ALP_LAST_ERROR_TLS alp_status_t z_last_error;

alp_status_t alp_last_error(void)
{
	return z_last_error;
}

void alp_internal_set_last_error(alp_status_t s)
{
	z_last_error = s;
}

/* ------------------------------------------------------------------ */
/* Delay primitives.                                                   */
/*                                                                     */
/* On a Linux host (the real Yocto target, and the ALP_SOM=none        */
/* "baremetal" plain-CMake build, which -- absent a vendor cross       */
/* toolchain file -- also compiles and runs natively on the CI host)   */
/* clock_nanosleep(CLOCK_MONOTONIC) gives an accurate, scheduler-       */
/* yielding wait; the loop below retries across EINTR (the request is  */
/* relative, so clock_nanosleep rewrites `ts` with the remaining time   */
/* on interruption) so a signal never truncates the sleep short of the  */
/* contract's "at least" floor.                                        */
/*                                                                     */
/* A genuine non-Linux bare-metal target (no vendor HAL delay override  */
/* exists yet -- see vendors/<som>/) has no clock to measure against,   */
/* so it falls through to a busy-loop.  The loop deliberately           */
/* over-provisions its per-microsecond iteration count rather than risk */
/* an early return; slower cores simply overshoot; "at least us elapses"*/
/* never becomes "well under us".  A vendor HAL bring-up should replace  */
/* it with a cycle-counter-driven wait (SysTick / DWT->CYCCNT / core     */
/* timer) once one lands.                                               */
/* ------------------------------------------------------------------ */

#if defined(__linux__)

#include <errno.h>
#include <time.h>

static void z_delay_clock_nanosleep(long sec, long nsec)
{
	struct timespec ts = { .tv_sec = sec, .tv_nsec = nsec };
	while (clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, &ts) == EINTR) {
		/* `ts` now holds the remaining time; retry until it elapses. */
	}
}

void alp_delay_us(uint32_t us)
{
	if (us == 0u) return;
	z_delay_clock_nanosleep((long)(us / 1000000u), (long)(us % 1000000u) * 1000L);
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	z_delay_clock_nanosleep((long)(ms / 1000u), (long)(ms % 1000u) * 1000000L);
}

#else /* !__linux__ -- no OS clock; fall back to an over-provisioned spin */

/* Deliberately large: chosen so even a multi-GHz core still spins for
 * at least 1 us per iteration of the outer loop below.  Overflow-safe
 * by construction -- the multiplication is bounded to one us worth of
 * spins per outer-loop pass instead of `us * SPINS_PER_US` in one shot. */
#define ALP_DELAY_STUB_SPINS_PER_US 100000u

void alp_delay_us(uint32_t us)
{
	for (uint32_t i = 0u; i < us; i++) {
		volatile uint32_t spin = ALP_DELAY_STUB_SPINS_PER_US;
		while (spin != 0u) {
			--spin;
		}
	}
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	for (uint32_t i = 0u; i < ms; i++) {
		alp_delay_us(1000u);
	}
}

#endif /* __linux__ */
