/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Cancellable bounded wait for Zephyr backends whose blocking call has
 * no cancel API (video_dequeue(), dmic_read(), k_sem_take() on a
 * semaphore shared between handles, ...).
 *
 * A concurrent close() sets the handle's `closing` flag (see
 * alp_handle_mark_closing() in alp_slot_claim.h); ALP_SLICED_WAIT() runs
 * the call in short slices and checks that flag between them, so the
 * waiter returns within one slice of the close.
 *
 * ponytail: latency ceiling = ALP_SLICE_WAIT_MS (50 ms) per cancelled
 * waiter, and one wake-up per slice (20 Hz) while blocked.  Upgrade path
 * per backend: a backend-owned k_sem/k_poll that cancel() gives.
 */

#ifndef ALP_SLICED_WAIT_H
#define ALP_SLICED_WAIT_H

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

#define ALP_SLICE_WAIT_MS 50u

/**
 * Run @p call repeatedly in slices of at most ALP_SLICE_WAIT_MS until it
 * succeeds / fails hard, the caller's @p timeout_ms elapses
 * (UINT32_MAX = forever) or @p closing_p becomes true.
 *
 * @p slice_ms names a uint32_t the macro sets before each @p call, so the
 * call can use it as its timeout.  @p rc names the caller's result
 * variable (assigned from @p call); @p is_timeout is a condition over
 * @p rc that means "the slice expired, nothing happened".
 *
 * Evaluates to true when the wait was cancelled by close (the call was
 * not made, or its result is stale); false otherwise, with @p rc holding
 * the call's own result (a timeout once the caller's budget is spent).
 */
#define ALP_SLICED_WAIT(closing_p, timeout_ms, slice_ms, rc, call, is_timeout) \
	({ \
		bool          _alp_cancelled = false; \
		bool          _alp_first     = true; \
		const bool    _alp_forever   = ((timeout_ms) == UINT32_MAX); \
		const int64_t _alp_deadline  = k_uptime_get() + (int64_t)(timeout_ms); \
		for (;;) { \
			if (__atomic_load_n((closing_p), __ATOMIC_ACQUIRE)) { \
				_alp_cancelled = true; \
				break; \
			} \
			int64_t _alp_left = \
			    _alp_forever ? (int64_t)ALP_SLICE_WAIT_MS : _alp_deadline - k_uptime_get(); \
			/* Budget spent between the last slice and now: report the \
			 * previous slice's timeout, never a 0 ms (K_NO_WAIT) call \
			 * whose "nothing yet" code is not the timeout code. */ \
			if (!_alp_first && !_alp_forever && _alp_left <= 0) { \
				break; \
			} \
			_alp_first = false; \
			if (_alp_left < 0) { \
				_alp_left = 0; \
			} \
			(slice_ms) = \
			    (uint32_t)(_alp_left > (int64_t)ALP_SLICE_WAIT_MS ? (int64_t)ALP_SLICE_WAIT_MS \
			                                                      : _alp_left); \
			(rc) = (call); \
			if (!(is_timeout)) { \
				break; \
			} \
			if (!_alp_forever && k_uptime_get() >= _alp_deadline) { \
				break; \
			} \
		} \
		_alp_cancelled; \
	})

#endif /* ALP_SLICED_WAIT_H */
