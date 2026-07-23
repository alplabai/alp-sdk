/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Portable "which thread am I" token (issue #756).
 *
 * A handle op that invokes an application callback SYNCHRONOUSLY,
 * inline, before returning to the dispatcher (alp_mqtt_loop()'s
 * message callback, alp_ble_scan_start()'s scan callback) can have
 * that very callback call close() on its OWN handle.  Draining
 * active_ops for such a close would deadlock: the calling thread's own
 * count cannot reach zero until IT returns, and it cannot return while
 * it is busy waiting inside close().  Distinguishing that reentrant
 * self-close from a genuine, safe-to-block-on close arriving from a
 * DIFFERENT thread needs a cheap, portable "is this the same thread"
 * check -- this header is that check, shared by every dispatcher that
 * needs it (src/mqtt_dispatch.c, src/ble_dispatch.c) instead of each
 * hand-rolling its own OS-specific thread-identity comparison.
 *
 * uintptr_t rather than exposing pthread_t/k_tid_t directly: both
 * underlying types are integral/pointer-sized on every OS this SDK
 * targets (Zephyr's k_tid_t is a `struct k_thread *`; glibc/musl's
 * pthread_t is an unsigned long / pointer), so the cast is lossless
 * and this header can be included from a portable dispatcher TU that
 * must build for both Zephyr and Yocto without carrying two different
 * field types in the same struct.
 */

#ifndef ALP_COMMON_THREAD_TOKEN_H
#define ALP_COMMON_THREAD_TOKEN_H

#include <stdint.h>

#if defined(__ZEPHYR__)
#include <zephyr/kernel.h>

static inline uintptr_t alp_thread_token_self(void)
{
	return (uintptr_t)k_current_get();
}
#elif defined(__linux__) || defined(__APPLE__)
#include <pthread.h>

static inline uintptr_t alp_thread_token_self(void)
{
	return (uintptr_t)pthread_self();
}
#else
/* Single-threaded / bare-metal target: there is only ever one thread,
 * so a synchronous callback's close() is always "the same thread" --
 * a constant satisfies that without pulling in an OS thread header. */
static inline uintptr_t alp_thread_token_self(void)
{
	return 1u;
}
#endif

#endif /* ALP_COMMON_THREAD_TOKEN_H */
