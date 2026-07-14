/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #760: the Yocto watchdog backend
 * (src/backends/wdt/yocto_drv.c) must attempt a best-effort disarm on
 * EVERY post-open failure (not just the ordinary close path), and its
 * millisecond-to-second ceiling division must not overflow uint32_t
 * for timeout_ms near UINT32_MAX.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/rpc_yocto_self_close.c) to reach its
 * file-local seams: g_wdt_test_open_hook / g_wdt_test_malloc_hook
 * (inject the allocation failure and hand back a test-controlled fd),
 * g_wdt_test_settimeout_observed_hook (observe the exact second value
 * y_open() programs, proving the overflow fix), and
 * g_wdt_test_disarm_observed_hook (prove WDIOS_DISABLECARD is
 * attempted on every failure path).  Opening a REAL /dev/watchdogN
 * would arm a genuine hardware watchdog on whatever host runs the
 * test -- this file never does that; every "device fd" here is a
 * pipe end or a deliberately-already-closed descriptor.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_wdt
 *   ctest --test-dir build -R alp_test_peripheral_wdt
 */

#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/backends/wdt/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static int  g_last_open_fd;
static int  g_disarm_call_count;
static bool g_disarm_last_magic;
static int  g_observed_timeout_s;

static void observe_disarm(int fd, bool magic_attempted)
{
	(void)fd;
	++g_disarm_call_count;
	g_disarm_last_magic = magic_attempted;
}

static void observe_settimeout(int timeout_s)
{
	g_observed_timeout_s = timeout_s;
}

/* Hands back a real, open pipe write-end -- a valid fd the ioctl()s in
 * y_open() can operate on (SETTIMEOUT/GETSUPPORT fail ENOTTY on a
 * pipe, which the backend already tolerates as "no such capability").
 * The read end is closed immediately, so any write() the backend later
 * performs against the write end (the magic-close 'V') deterministically
 * fails EPIPE -- harmless with SIGPIPE ignored (see main()) and exercises
 * that write()'s error path too. */
static int open_hook_pipe_fd(const char *path)
{
	(void)path;
	int fds[2];
	if (pipe(fds) != 0) return -1;
	(void)close(fds[0]);
	g_last_open_fd = fds[1];
	return fds[1];
}

/* Hands back a numerically-valid but already-closed fd, so every
 * ioctl()/write()/close() the backend performs against it deterministically
 * fails with EBADF -- used to force WDIOC_SETTIMEOUT's hard-failure branch
 * without needing a real watchdog device. */
static int open_hook_precloses_fd(const char *path)
{
	(void)path;
	int fd = open("/dev/null", O_WRONLY | O_CLOEXEC);
	if (fd < 0) return -1;
	(void)close(fd);
	g_last_open_fd = fd;
	return fd;
}

static void *malloc_hook_always_fails(size_t size)
{
	(void)size;
	return NULL;
}

static void reset_fixture(void)
{
	g_last_open_fd                      = -1;
	g_disarm_call_count                 = 0;
	g_disarm_last_magic                 = false;
	g_observed_timeout_s                = -1;
	g_wdt_test_open_hook                = NULL;
	g_wdt_test_malloc_hook              = NULL;
	g_wdt_test_settimeout_observed_hook = observe_settimeout;
	g_wdt_test_disarm_observed_hook     = observe_disarm;
}

static alp_wdt_config_t default_cfg(uint32_t timeout_ms)
{
	return (alp_wdt_config_t){
		.wdt_id     = 0u,
		.timeout_ms = timeout_ms,
		.on_timeout = ALP_WDT_RESET_SOC,
	};
}

/* ------------------------------------------------------------------ */

static void test_malloc_failure_disarms_and_closes_fd(void)
{
	reset_fixture();
	g_wdt_test_open_hook   = open_hook_pipe_fd;
	g_wdt_test_malloc_hook = malloc_hook_always_fails;

	alp_wdt_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	alp_wdt_config_t        cfg  = default_cfg(5000u);

	alp_status_t rc = y_open(&cfg, &st, &caps);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOMEM);
	ALP_ASSERT_EQ_INT(g_disarm_call_count, 1);
	ALP_ASSERT_TRUE(!g_disarm_last_magic); /* GETSUPPORT never ran -- unknown, not attempted */

	ALP_ASSERT_TRUE(g_last_open_fd >= 0);
	errno = 0;
	ALP_ASSERT_TRUE(fcntl(g_last_open_fd, F_GETFD) < 0 && errno == EBADF);
}

static void test_settimeout_hard_failure_disarms(void)
{
	reset_fixture();
	g_wdt_test_open_hook = open_hook_precloses_fd;

	alp_wdt_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	alp_wdt_config_t        cfg  = default_cfg(5000u);

	alp_status_t rc = y_open(&cfg, &st, &caps);
	ALP_ASSERT_TRUE(rc != ALP_OK); /* EBADF on the pre-closed fd, not tolerated */
	ALP_ASSERT_EQ_INT(g_disarm_call_count, 1);
	ALP_ASSERT_TRUE(!g_disarm_last_magic);
}

static void test_timeout_ms_uint32_max_does_not_overflow(void)
{
	reset_fixture();
	g_wdt_test_open_hook = open_hook_pipe_fd;

	alp_wdt_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	alp_wdt_config_t        cfg  = default_cfg(UINT32_MAX);

	alp_status_t rc = y_open(&cfg, &st, &caps);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	/* ceil(UINT32_MAX / 1000) = 4294968 (~49.7 days); the pre-fix
     * `(timeout_ms + 999u) / 1000u` wraps uint32_t and programs 1. */
	ALP_ASSERT_EQ_INT(g_observed_timeout_s, 4294968);

	y_close(&st);
}

static void test_close_attempts_magic_write_when_supported(void)
{
	reset_fixture();
	g_wdt_test_open_hook = open_hook_pipe_fd;

	alp_wdt_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	alp_wdt_config_t        cfg  = default_cfg(1000u);

	alp_status_t rc = y_open(&cfg, &st, &caps);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);

	/* GETSUPPORT failed (ENOTTY on a pipe) so magic_close is false
     * post-open; force it as if the real driver had advertised
     * WDIOF_MAGICCLOSE, to prove y_close() honours it. */
	((y_wdt_data_t *)st.be_data)->magic_close = true;

	g_disarm_call_count = 0;
	y_close(&st);
	ALP_ASSERT_EQ_INT(g_disarm_call_count, 1);
	ALP_ASSERT_TRUE(g_disarm_last_magic);
}

int main(void)
{
	/* The read end of every fake "device" pipe is closed immediately
     * (see open_hook_pipe_fd), so the magic-close write() in
     * _disarm_and_close() deterministically hits EPIPE -- ignore
     * SIGPIPE so that surfaces as a normal write() error instead of
     * killing the test process. */
	(void)signal(SIGPIPE, SIG_IGN);

	test_malloc_failure_disarms_and_closes_fd();
	test_settimeout_hard_failure_disarms();
	test_timeout_ms_uint32_max_does_not_overflow();
	test_close_attempts_magic_write_when_supported();

	ALP_TEST_SUMMARY();
}
