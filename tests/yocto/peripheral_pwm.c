/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #744: the Yocto PWM sysfs backend
 * (src/backends/pwm/yocto_drv.c) must not unexport, disable, or
 * reconfigure a channel it did not itself export.  EBUSY from the
 * sysfs `export` write means another process already owns the
 * channel, and every open/rollback/close path must leave that
 * channel's export -- and its polarity/period/duty_cycle/enable
 * configuration -- intact.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/rpc_yocto_self_close.c) so it can drive
 * y_open()/y_close() through g_pwm_test_sysfs_write_hook -- a canned
 * responder standing in for every `/sys/class/pwm/...` attribute write
 * this backend performs -- instead of a real pwmchip sysfs node.
 * There usually isn't one in CI, and even where a real /sys/class/pwm
 * exists on a dev host (as it does on this box, backing the Intel PCH
 * PWM), poking it would touch a genuine, non-alp-sdk-target peripheral
 * (backlight/fan/etc) -- exactly what a test must never do.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_pwm
 *   ctest --test-dir build -R alp_test_peripheral_pwm
 */

#include <stdint.h>
#include <string.h>

#include "test_assert.h"

#include "../../src/backends/pwm/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture: records every (path, val) the hook observed and lets each  */
/* test script the per-path status the fake "sysfs write" returns.     */
/* ------------------------------------------------------------------ */

#define MAX_CALLS 16

static char         g_call_paths[MAX_CALLS][128];
static char         g_call_vals[MAX_CALLS][16];
static int          g_call_count;
static alp_status_t g_export_result; /* what the "export" write returns */
static bool         g_fail_period;   /* force the "period" write to fail */

static alp_status_t fake_sysfs_write(const char *path, const char *val)
{
	if (g_call_count < MAX_CALLS) {
		(void)snprintf(g_call_paths[g_call_count], sizeof(g_call_paths[g_call_count]), "%s", path);
		(void)snprintf(g_call_vals[g_call_count], sizeof(g_call_vals[g_call_count]), "%s", val);
		++g_call_count;
	}
	bool is_unexport = strstr(path, "/unexport") != NULL;
	bool is_export   = !is_unexport && strstr(path, "/export") != NULL;
	if (is_export) return g_export_result;
	if (is_unexport) return ALP_OK;
	if (g_fail_period && strstr(path, "/period") != NULL) return ALP_ERR_IO;
	return ALP_OK;
}

static bool saw_unexport(void)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strstr(g_call_paths[i], "/unexport") != NULL) return true;
	}
	return false;
}

/* True if a write of "0" landed on the per-channel enable attribute --
 * i.e. the PWM output was disabled. */
static bool saw_disable(void)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strstr(g_call_paths[i], "/enable") != NULL && strcmp(g_call_vals[i], "0") == 0) {
			return true;
		}
	}
	return false;
}

/* True if any per-channel configuration attribute (polarity, period,
 * duty_cycle or enable) was written -- i.e. the channel was set up or
 * touched beyond the bare export/unexport request. */
static bool saw_configure_write(void)
{
	for (int i = 0; i < g_call_count; ++i) {
		if (strstr(g_call_paths[i], "/polarity") != NULL) return true;
		if (strstr(g_call_paths[i], "/period") != NULL) return true;
		if (strstr(g_call_paths[i], "/duty_cycle") != NULL) return true;
		if (strstr(g_call_paths[i], "/enable") != NULL) return true;
	}
	return false;
}

static void reset_fixture(void)
{
	g_call_count                = 0;
	g_export_result             = ALP_OK;
	g_fail_period               = false;
	g_pwm_test_sysfs_write_hook = fake_sysfs_write;
}

static alp_status_t open_handle(struct alp_pwm *h, alp_pwm_backend_state_t **st_out)
{
	memset(h, 0, sizeof(*h));
	alp_pwm_config_t cfg = {
		.channel_id = 0u,
		.period_ns  = 1000000u,
		.polarity   = ALP_PWM_POLARITY_NORMAL,
	};
	alp_capabilities_t caps = { 0 };
	*st_out                 = &h->state;
	return y_open(&cfg, &h->state, &caps);
}

/* ------------------------------------------------------------------ */

static void test_new_export_owns_and_close_unexports(void)
{
	reset_fixture();
	struct alp_pwm           h;
	alp_pwm_backend_state_t *st;
	alp_status_t             rc = open_handle(&h, &st);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);

	y_pwm_data_t *d = (y_pwm_data_t *)st->be_data;
	ALP_ASSERT_TRUE(d->owns_export);

	g_call_count = 0; /* isolate what close() itself does */
	y_close(st);
	ALP_ASSERT_TRUE(saw_unexport());
	ALP_ASSERT_TRUE(saw_disable());
}

static void test_already_exported_does_not_own_and_close_leaves_export(void)
{
	reset_fixture();
	g_export_result = ALP_ERR_BUSY;

	struct alp_pwm           h;
	alp_pwm_backend_state_t *st;
	alp_status_t             rc = open_handle(&h, &st);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);

	y_pwm_data_t *d = (y_pwm_data_t *)st->be_data;
	ALP_ASSERT_TRUE(!d->owns_export);

	g_call_count = 0;
	y_close(st);
	ALP_ASSERT_TRUE(!saw_unexport());
}

/* Regression for the CHANGES_REQUIRED finding: y_close() must not write
 * enable="0" for a handle that merely reused a foreign export (EBUSY at
 * open) -- doing so disables another process's PWM output even though
 * this handle never claimed the channel. */
static void test_already_exported_close_does_not_disable(void)
{
	reset_fixture();
	g_export_result = ALP_ERR_BUSY;

	struct alp_pwm           h;
	alp_pwm_backend_state_t *st;
	alp_status_t             rc = open_handle(&h, &st);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);

	y_pwm_data_t *d = (y_pwm_data_t *)st->be_data;
	ALP_ASSERT_TRUE(!d->owns_export);

	g_call_count = 0; /* isolate what close() itself does */
	y_close(st);
	ALP_ASSERT_TRUE(!saw_disable());
}

static void test_post_export_failure_new_export_unexports(void)
{
	reset_fixture();
	g_fail_period = true;

	struct alp_pwm           h;
	alp_pwm_backend_state_t *st;
	alp_status_t             rc = open_handle(&h, &st);
	ALP_ASSERT_TRUE(rc != ALP_OK);
	ALP_ASSERT_TRUE(saw_unexport());
}

/* Regression for the round-3 finding: y_open() must leave a reused
 * (EBUSY) channel's polarity/period/duty_cycle/enable completely
 * untouched, not just skip the unexport.  Without the ownership gate
 * added around those writes, open() would zero the duty, rewrite the
 * period, and force-enable a channel another process already
 * configured -- which is precisely the "left ... exactly as that other
 * owner set it up" state y_close()'s doc comment claims.  This test is
 * what makes that claim true on the open side (y_close's own tests
 * above cover the close side).  g_fail_period stays set so a mutation
 * that drops the gate (making open() attempt the period write again)
 * is also caught here: the forced IO failure would surface as
 * rc != ALP_OK, which this test rejects too. */
static void test_already_exported_open_does_not_configure(void)
{
	reset_fixture();
	g_export_result = ALP_ERR_BUSY;
	g_fail_period   = true;

	struct alp_pwm           h;
	alp_pwm_backend_state_t *st;
	alp_status_t             rc = open_handle(&h, &st);
	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_TRUE(!saw_configure_write());

	y_close(st);
}

int main(void)
{
	test_new_export_owns_and_close_unexports();
	test_already_exported_does_not_own_and_close_leaves_export();
	test_already_exported_close_does_not_disable();
	test_post_export_failure_new_export_unexports();
	test_already_exported_open_does_not_configure();

	ALP_TEST_SUMMARY();
}
