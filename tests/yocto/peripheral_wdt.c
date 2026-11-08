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
 * g_wdt_test_setoptions_hook (proves WDIOS_DISABLECARD is attempted on
 * every failure path).  It stands in for the syscall rather than
 * watching from beside it, so the assertions below cannot pass unless
 * the disarm actually ran; a hook
 * placed alongside the call would stay green if the call were deleted,
 * which is the failure mode this suite exists to prevent.  Opening a
 * REAL /dev/watchdogN
 * would arm a genuine hardware watchdog on whatever host runs the
 * test -- this file never does that; every "device fd" here is a
 * pipe end, a temp file, or a deliberately-already-closed descriptor.
 *
 * #2981: a successful close must NOT disarm.  The "fake /dev/watchdog"
 * for that is a temp file; the test asserts no 'V' byte lands in it and
 * no WDIOS_DISABLECARD is issued.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_wdt
 *   ctest --test-dir build -R alp_test_peripheral_wdt
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/backends/wdt/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static int g_last_open_fd;
static int g_disarm_call_count;
static int g_observed_timeout_s;

/* Stands in for the WDIOS_DISABLECARD ioctl itself, so the count can
 * only rise when the backend genuinely issued one -- delete the disarm
 * and this is never reached.  Returns 0 (success) as a real driver
 * would; the production caller discards it either way. */
static int fake_setoptions(int fd, int flags)
{
	(void)fd;
	if (flags == WDIOS_DISABLECARD) {
		++g_disarm_call_count;
	}
	return 0;
}

static void observe_settimeout(int timeout_s)
{
	g_observed_timeout_s = timeout_s;
}

/* Hands back a real, open pipe write-end -- a valid fd the ioctl()s in
 * y_open() can operate on (SETTIMEOUT/GETSUPPORT fail ENOTTY on a
 * pipe, which the backend already tolerates as "no such capability").
 * The read end is closed immediately so the fd is never left readable.
 */
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
	g_observed_timeout_s                = -1;
	g_wdt_test_open_hook                = NULL;
	g_wdt_test_malloc_hook              = NULL;
	g_wdt_test_settimeout_observed_hook = observe_settimeout;
	g_wdt_test_setoptions_hook          = fake_setoptions;
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

static int g_tmp_dup = -1; /* survives y_close() so the file can be inspected */

static int open_hook_tmpfile(const char *path)
{
	(void)path;
	char tmpl[] = "/tmp/alp_wdt_fake_XXXXXX";
	int  fd     = mkstemp(tmpl);
	if (fd < 0) return -1;
	(void)unlink(tmpl);
	g_tmp_dup = dup(fd);
	return fd;
}

/* #2981: y_close() must neither write the magic 'V' nor issue
 * WDIOS_DISABLECARD.  The fake device is a real temp file, so a
 * regressed close leaves a 'V' byte in it (no write seam to hide
 * behind). */
static void test_close_never_disarms(void)
{
	reset_fixture();
	g_wdt_test_open_hook = open_hook_tmpfile;

	alp_wdt_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	alp_wdt_config_t        cfg  = default_cfg(1000u);

	ALP_ASSERT_EQ_INT(y_open(&cfg, &st, &caps), ALP_OK);

	g_disarm_call_count = 0;
	y_close(&st);
	ALP_ASSERT_EQ_INT(g_disarm_call_count, 0);

	char    buf[4];
	ssize_t n = pread(g_tmp_dup, buf, sizeof(buf), 0);
	ALP_ASSERT_EQ_INT((int)n, 0); /* nothing, in particular no 'V', was written */
	(void)close(g_tmp_dup);
}

static void test_interrupt_only_rejected_before_any_syscall(void)
{
	/* #1637: the Linux watchdog ABI has no expiry-notification ioctl,
     * so on_expire could never fire on this backend -- y_open() must
     * reject INTERRUPT_ONLY outright rather than silently arming a
     * watchdog whose configured action can never be observed.  The
     * open hook, if reached, would set g_last_open_fd -- leaving it
     * wired (rather than NULL) is what makes "still -1 after the
     * call" proof that open() was never attempted, not a NULL-hook
     * coincidence. */
	reset_fixture();
	g_wdt_test_open_hook = open_hook_pipe_fd;

	alp_wdt_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	alp_wdt_config_t        cfg  = { .wdt_id     = 0u,
		                             .timeout_ms = 1000u,
		                             .on_timeout = ALP_WDT_INTERRUPT_ONLY };

	alp_status_t rc = y_open(&cfg, &st, &caps);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(g_last_open_fd, -1); /* the wired hook never ran */
}

static void test_window_and_pause_rejected_before_any_syscall(void)
{
	/* #1637: WDIOC has no window or sleep/debug pause control, so both
	 * are refused up front instead of arming a plain watchdog. */
	alp_wdt_config_t cfgs[3] = { default_cfg(1000u), default_cfg(1000u), default_cfg(1000u) };
	cfgs[0].window_min_ms    = 100u;
	cfgs[1].flags            = ALP_WDT_PAUSE_IN_SLEEP;
	cfgs[2].flags            = ALP_WDT_PAUSE_HALTED_BY_DEBUG;
	for (size_t i = 0; i < 3u; i++) {
		reset_fixture();
		g_wdt_test_open_hook         = open_hook_pipe_fd;
		alp_wdt_backend_state_t st   = { 0 };
		alp_capabilities_t      caps = { 0 };
		ALP_ASSERT_EQ_INT(y_open(&cfgs[i], &st, &caps), ALP_ERR_NOSUPPORT);
		ALP_ASSERT_EQ_INT(g_last_open_fd, -1); /* the wired hook never ran */
	}
}

int main(void)
{
	/* Backstop: a stray write to a pipe whose read end is closed must
     * surface EPIPE instead of killing the process. */
	(void)signal(SIGPIPE, SIG_IGN);

	test_malloc_failure_disarms_and_closes_fd();
	test_settimeout_hard_failure_disarms();
	test_timeout_ms_uint32_max_does_not_overflow();
	test_close_never_disarms();
	test_interrupt_only_rejected_before_any_syscall();
	test_window_and_pause_rejected_before_any_syscall();

	ALP_TEST_SUMMARY();
}
