/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #613: the Yocto power backend
 * (src/backends/power/yocto_drv.c) must map each portable
 * alp_power_mode_t to the EXACT /sys/power/state token the kernel ABI
 * expects, must program the RTC wakealarm (clear-then-set) whenever a
 * timed wake is requested, and must surface a sysfs write failure as
 * an explicit ALP_ERR_* rather than swallowing it into ALP_OK.
 *
 * Before this backend existed, request_sleep() was only ever served
 * by src/backends/power/zephyr_stub.c, which returns ALP_ERR_NOSUPPORT
 * and writes nothing -- this file's g_power_test_sysfs_write_hook /
 * g_power_test_time_hook seams do not exist on that pre-feature code,
 * so this test fails to even COMPILE against it (proof: build this
 * target against a checkout before src/backends/power/yocto_drv.c was
 * added and the #include below fails with "No such file or
 * directory").  On the real backend it drives y_request_sleep()
 * against a canned sysfs-write responder instead of the real kernel
 * PM/RTC nodes -- writing to a real /sys/power/state would actually
 * suspend whatever host runs the test, which a test must never do.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_power
 *   ctest --test-dir build -R alp_test_peripheral_power
 */

#include <stdint.h>
#include <string.h>

#include "test_assert.h"

#include "../../src/backends/power/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture: records every (path, val) the hook observed and lets each  */
/* test script the per-path status the fake "sysfs write" returns.     */
/* ------------------------------------------------------------------ */

#define MAX_CALLS 8

static char g_call_paths[MAX_CALLS][96];
static char g_call_vals[MAX_CALLS][32];
static int  g_call_count;
static bool g_fail_state_write;
static bool g_fail_wakealarm_write;

static alp_status_t fake_sysfs_write(const char *path, const char *val)
{
	if (g_call_count < MAX_CALLS) {
		(void)snprintf(g_call_paths[g_call_count], sizeof(g_call_paths[g_call_count]), "%s", path);
		(void)snprintf(g_call_vals[g_call_count], sizeof(g_call_vals[g_call_count]), "%s", val);
		++g_call_count;
	}
	if (g_fail_state_write && strcmp(path, ALP_YOCTO_POWER_STATE_PATH) == 0) return ALP_ERR_IO;
	if (g_fail_wakealarm_write && strcmp(path, ALP_YOCTO_POWER_WAKEALARM_PATH) == 0) {
		return ALP_ERR_IO;
	}
	return ALP_OK;
}

static time_t fake_time(void)
{
	return (time_t)1000000000; /* fixed epoch so the target write is exact */
}

static void reset_fixture(void)
{
	g_call_count                  = 0;
	g_fail_state_write            = false;
	g_fail_wakealarm_write        = false;
	g_power_test_sysfs_write_hook = fake_sysfs_write;
	g_power_test_time_hook        = fake_time;
}

static bool call_at(int i, const char *path, const char *val)
{
	return i < g_call_count && strcmp(g_call_paths[i], path) == 0 &&
	       strcmp(g_call_vals[i], val) == 0;
}

static bool saw_call(const char *path, const char *val)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strcmp(g_call_paths[i], path) == 0 && strcmp(g_call_vals[i], val) == 0) return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */

static void test_sleep_mode_writes_freeze(void)
{
	reset_fixture();
	alp_power_backend_state_t st   = { 0 };
	alp_power_wake_info_t     info = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_SLEEP, 0u, &info);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_EQ_INT(g_call_count, 1);
	ALP_ASSERT_TRUE(call_at(0, ALP_YOCTO_POWER_STATE_PATH, "freeze"));
	ALP_ASSERT_EQ_INT((int)info.realised_mode, ALP_POWER_MODE_SLEEP);
	ALP_ASSERT_EQ_INT((int)info.wake_source, 0);
}

static void test_deep_sleep_writes_standby(void)
{
	reset_fixture();
	alp_power_backend_state_t st = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_DEEP_SLEEP, 0u, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(saw_call(ALP_YOCTO_POWER_STATE_PATH, "standby"));
}

static void test_standby_writes_mem(void)
{
	reset_fixture();
	alp_power_backend_state_t st = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_STANDBY, 0u, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(saw_call(ALP_YOCTO_POWER_STATE_PATH, "mem"));
}

/* wake_after_ms > 0 must clear the wakealarm ("0") THEN write the
 * absolute target epoch, ceiling-rounded to whole seconds, before the
 * /sys/power/state write. fake_time() pins "now" at 1000000000, so
 * 5000 ms -> ceil(5000/1000) = 5 s -> target 1000000005 exactly. */
static void test_wake_after_ms_programs_wakealarm(void)
{
	reset_fixture();
	alp_power_backend_state_t st   = { 0 };
	alp_power_wake_info_t     info = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_SLEEP, 5000u, &info);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_EQ_INT(g_call_count, 3);
	ALP_ASSERT_TRUE(call_at(0, ALP_YOCTO_POWER_WAKEALARM_PATH, "0"));
	ALP_ASSERT_TRUE(call_at(1, ALP_YOCTO_POWER_WAKEALARM_PATH, "1000000005"));
	ALP_ASSERT_TRUE(call_at(2, ALP_YOCTO_POWER_STATE_PATH, "freeze"));
	ALP_ASSERT_EQ_INT((int)info.wake_source, ALP_POWER_WAKE_RTC);
}

/* A wake_after_ms that isn't already a whole number of seconds must
 * round UP, never down (a short sleep must never present as
 * "already expired"). 1500 ms -> ceil = 2 s. */
static void test_wake_after_ms_rounds_up(void)
{
	reset_fixture();
	alp_power_backend_state_t st = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_SLEEP, 1500u, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(call_at(1, ALP_YOCTO_POWER_WAKEALARM_PATH, "1000000002"));
}

/* A failed /sys/power/state write must surface as the mapped
 * ALP_ERR_* -- not be silently swallowed into ALP_OK. */
static void test_state_write_failure_is_not_swallowed(void)
{
	reset_fixture();
	g_fail_state_write           = true;
	alp_power_backend_state_t st = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_SLEEP, 0u, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_IO);
}

/* A failed wakealarm write must abort BEFORE the /sys/power/state
 * write (never sleep with an un-programmed wake source silently) and
 * must surface the error rather than swallow it. */
static void test_wakealarm_failure_aborts_before_state_write(void)
{
	reset_fixture();
	g_fail_wakealarm_write       = true;
	alp_power_backend_state_t st = { 0 };

	alp_status_t rc = y_request_sleep(&st, ALP_POWER_MODE_SLEEP, 5000u, NULL);
	ALP_ASSERT_EQ_INT(rc, ALP_ERR_IO);
	ALP_ASSERT_TRUE(!saw_call(ALP_YOCTO_POWER_STATE_PATH, "freeze"));
}

int main(void)
{
	test_sleep_mode_writes_freeze();
	test_deep_sleep_writes_standby();
	test_standby_writes_mem();
	test_wake_after_ms_programs_wakealarm();
	test_wake_after_ms_rounds_up();
	test_state_write_failure_is_not_swallowed();
	test_wakealarm_failure_aborts_before_state_write();

	ALP_TEST_SUMMARY();
}
