/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #613: the Yocto power sysfs backend
 * (src/backends/power/yocto_drv.c) must map ALP_POWER_MODE_SLEEP /
 * ALP_POWER_MODE_DEEP_SLEEP to the documented `/sys/power/state`
 * tokens ("freeze" / "mem"), refuse ALP_POWER_MODE_STANDBY without
 * writing anything (Linux "mem" retains RAM; STANDBY's contract is
 * "RAM NOT retained" -- see the backend's own header comment), and
 * sequence the rtc0 wakealarm as clear-then-arm before sleeping and
 * clear again after wake.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_pwm.c) so it can drive
 * y_request_sleep() through g_power_test_sysfs_write_hook -- a canned
 * responder standing in for every sysfs attribute write this backend
 * performs -- instead of a real /sys/power/state or rtc0 wakealarm
 * node.  Never poke either on a dev host: /sys/power/state is a
 * genuine, non-alp-sdk-target system control.
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
/* Fixture: records every (path, val) the hook observed, in order, and */
/* lets each test force a given attribute's write to fail.              */
/* ------------------------------------------------------------------ */

#define MAX_CALLS 16

static char g_call_paths[MAX_CALLS][128];
static char g_call_vals[MAX_CALLS][32];
static int  g_call_count;
static bool g_fail_state; /* force the /power/state write to fail */

static alp_status_t fake_sysfs_write(const char *path, const char *val)
{
	if (g_call_count < MAX_CALLS) {
		(void)snprintf(g_call_paths[g_call_count], sizeof(g_call_paths[g_call_count]), "%s", path);
		(void)snprintf(g_call_vals[g_call_count], sizeof(g_call_vals[g_call_count]), "%s", val);
		++g_call_count;
	}
	bool is_state = strstr(path, "/power/state") != NULL;
	if (is_state && g_fail_state) return ALP_ERR_IO;
	return ALP_OK;
}

static void reset_fixture(void)
{
	g_call_count                  = 0;
	g_fail_state                  = false;
	g_power_test_sysfs_write_hook = fake_sysfs_write;
}

static int find_call(const char *path_substr, const char *val)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strstr(g_call_paths[i], path_substr) != NULL && strcmp(g_call_vals[i], val) == 0) {
			return i;
		}
	}
	return -1;
}

static bool saw_wakealarm_write(void)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strstr(g_call_paths[i], "wakealarm") != NULL) return true;
	}
	return false;
}

static bool saw_state_write(void)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strstr(g_call_paths[i], "/power/state") != NULL) return true;
	}
	return false;
}

/* ------------------------------------------------------------------ */

static void test_sleep_writes_freeze_no_wakealarm(void)
{
	reset_fixture();
	alp_power_wake_info_t info = { 0 };
	alp_status_t          rc   = y_request_sleep(NULL, ALP_POWER_MODE_SLEEP, 0u, &info);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	int idx = find_call("/power/state", "freeze");
	ALP_ASSERT_TRUE(idx >= 0);
	ALP_ASSERT_TRUE(!saw_wakealarm_write());
	ALP_ASSERT_EQ_INT((int)info.realised_mode, (int)ALP_POWER_MODE_SLEEP);
	ALP_ASSERT_EQ_INT((int)info.wake_source, 0);
}

static void test_deep_sleep_writes_mem(void)
{
	reset_fixture();
	alp_power_wake_info_t info = { 0 };
	alp_status_t          rc   = y_request_sleep(NULL, ALP_POWER_MODE_DEEP_SLEEP, 0u, &info);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(find_call("/power/state", "mem") >= 0);
	ALP_ASSERT_EQ_INT((int)info.realised_mode, (int)ALP_POWER_MODE_DEEP_SLEEP);
}

/* Standby must be refused before touching any sysfs attribute. */
static void test_standby_returns_nosupport_writes_nothing(void)
{
	reset_fixture();
	alp_power_wake_info_t info = { 0 };
	alp_status_t          rc   = y_request_sleep(NULL, ALP_POWER_MODE_STANDBY, 5000u, &info);

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(g_call_count, 0);
	ALP_ASSERT_EQ_INT((int)info.realised_mode, (int)ALP_POWER_MODE_STANDBY);
}

/* With a non-zero wake_after_ms, the wakealarm must be cleared ("0"),
 * then armed with an absolute epoch, BEFORE the state write, and
 * cleared again AFTER it. */
static void test_wake_after_ms_arms_and_clears_wakealarm_in_order(void)
{
	reset_fixture();
	alp_power_wake_info_t info = { 0 };
	alp_status_t          rc   = y_request_sleep(NULL, ALP_POWER_MODE_DEEP_SLEEP, 30000u, &info);

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(g_call_count >= 4);

	/* [0] clear, [1] arm (non-"0" epoch), [2] state write, [3] clear. */
	ALP_ASSERT_TRUE(strstr(g_call_paths[0], "wakealarm") != NULL);
	ALP_ASSERT_TRUE(strcmp(g_call_vals[0], "0") == 0);

	ALP_ASSERT_TRUE(strstr(g_call_paths[1], "wakealarm") != NULL);
	ALP_ASSERT_TRUE(strcmp(g_call_vals[1], "0") != 0); /* an epoch, not a clear */

	ALP_ASSERT_TRUE(strstr(g_call_paths[2], "/power/state") != NULL);
	ALP_ASSERT_TRUE(strcmp(g_call_vals[2], "mem") == 0);

	ALP_ASSERT_TRUE(strstr(g_call_paths[3], "wakealarm") != NULL);
	ALP_ASSERT_TRUE(strcmp(g_call_vals[3], "0") == 0);

	ALP_ASSERT_EQ_INT((int)info.wake_source, (int)ALP_POWER_WAKE_RTC);
}

/* A failed state write must still clear an armed wakealarm (no stray
 * alarm left behind after a failed suspend), and must propagate the
 * failure rather than silently succeeding. */
static void test_state_write_failure_still_clears_wakealarm_and_propagates(void)
{
	reset_fixture();
	g_fail_state = true;

	alp_power_wake_info_t info = { 0 };
	alp_status_t          rc   = y_request_sleep(NULL, ALP_POWER_MODE_SLEEP, 1000u, &info);

	ALP_ASSERT_TRUE(rc != ALP_OK);
	ALP_ASSERT_TRUE(saw_state_write());
	/* Last recorded call must be the post-failure wakealarm clear. */
	ALP_ASSERT_TRUE(g_call_count >= 1);
	int last = g_call_count - 1;
	ALP_ASSERT_TRUE(strstr(g_call_paths[last], "wakealarm") != NULL);
	ALP_ASSERT_TRUE(strcmp(g_call_vals[last], "0") == 0);
}

int main(void)
{
	test_sleep_writes_freeze_no_wakealarm();
	test_deep_sleep_writes_mem();
	test_standby_returns_nosupport_writes_nothing();
	test_wake_after_ms_arms_and_clears_wakealarm_in_order();
	test_state_write_failure_still_clears_wakealarm_and_propagates();

	ALP_TEST_SUMMARY();
}
