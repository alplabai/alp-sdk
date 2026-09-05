/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared core of the split stub backend for the Alp SDK: the one
 * canonical last-error slot and the delay/uptime primitives.  Split
 * out of the former src/common/stub_backend.c monolith (issue #673)
 * -- every sibling `stub_<class>.c` in this directory owns the
 * NOSUPPORT body for one public API class; this TU owns the pieces
 * of shared state/behaviour every backend needs regardless of class:
 * `alp_last_error()`, `alp_delay_us`/`alp_delay_ms`, and
 * `alp_uptime_ms` (issue #1953).
 *
 * Backends that do real work (currently `src/zephyr/`) override
 * selectively via per-class Kconfig and CMake gating; backends
 * without a working impl yet (`src/baremetal/`, `src/yocto/`)
 * compile the full set of `src/common/stub/` sources so the
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

/* Yocto's alp_uptime_ms (issue #1953): CLOCK_MONOTONIC read straight
 * through, same clock the delay primitives above measure against, so
 * `alp_uptime_ms()` readings and `alp_delay_ms()` waits agree with each
 * other. */
uint64_t alp_uptime_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

#else /* !__linux__ -- no OS clock; fall back to an over-provisioned spin */

/* Deliberately large: chosen so even a multi-GHz core still spins for
 * at least 1 us per iteration of the outer loop below.  Overflow-safe
 * by construction -- the multiplication is bounded to one us worth of
 * spins per outer-loop pass instead of `us * SPINS_PER_US` in one shot. */
#define ALP_DELAY_STUB_SPINS_PER_US 100000u

/* ponytail: no real timer exists on this path, so alp_uptime_ms() can only
 * total the durations THIS backend has itself spun for via alp_delay_us/ms
 * below -- it cannot see time spent anywhere else (a raw vendor-HAL wait
 * that bypasses these calls, e.g.).  That is still monotonic and internally
 * consistent, which is all chips/cc3501e/cc3501e_core.c's poll_by_repeat()
 * deadline math (issue #1953) needs from THIS backend: every wait it takes
 * is one of the alp_delay_ms() calls below.  Ceiling: a target with real
 * hardware-timing work outside alp_delay_* would need a genuine cycle-
 * counter read (SysTick / DWT->CYCCNT / core timer) once a vendor HAL
 * bring-up lands -- same upgrade path the delay spin above already notes.
 * No non-Linux target builds this file today (the plain-CMake baremetal
 * config compiles + runs on the Linux CI host absent a cross toolchain, so
 * it hits the __linux__ branch above instead) -- this is the dormant path
 * a genuine bare-metal port will exercise first.
 *
 * Accumulated in MICROSECONDS, not milliseconds: cc3501e_core.c's
 * cc3501e_reply_gate() settles on sub-millisecond alp_delay_us() calls
 * (e.g. CC3501E_READY_POLL_US), and a millisecond-granularity counter would
 * truncate every one of those to zero -- an unbounded undercount over a
 * long poll, not a rounding nit, since a caller that never sleeps a whole
 * millisecond at once would see alp_uptime_ms() stand still forever. */
static uint64_t z_uptime_stub_us;

void alp_delay_us(uint32_t us)
{
	for (uint32_t i = 0u; i < us; i++) {
		volatile uint32_t spin = ALP_DELAY_STUB_SPINS_PER_US;
		while (spin != 0u) {
			--spin;
		}
	}
	z_uptime_stub_us += us;
}

void alp_delay_ms(uint32_t ms)
{
	if (ms == 0u) return;
	for (uint32_t i = 0u; i < ms; i++) {
		alp_delay_us(1000u);
	}
}

uint64_t alp_uptime_ms(void)
{
	return z_uptime_stub_us / 1000u;
}

#endif /* __linux__ */
