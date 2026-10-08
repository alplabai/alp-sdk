/* SPDX-License-Identifier: Apache-2.0 */
#include <zephyr/ztest.h>

#include <alp/peripheral.h>
#include <alp/update_log.h>

#include "../../../../src/backends/update_log/update_log_ops.h"

/*
 * CONFIG_ALP_SDK_UPDATE_LOG_REQUIRE_HW_ENFORCED=y (issue #111, phase P1).
 *
 * This build keeps the software tamper-evident tier registered (priority 10)
 * but forbids the dispatcher from ever binding it. A stand-in HW_ENFORCED
 * backend (priority 20) plays the trusted owner; its ready() verdict models
 * the three real states: owner answers (ALP_OK), owner absent
 * (ALP_ERR_NOSUPPORT), and a hard fault. The firewall is not modelled: it is
 * asserted by the build, not checked at runtime.
 *
 * The property under test: with the requirement set, an absent
 * owner makes alp_update_log_open() return NULL with ALP_ERR_NOSUPPORT --
 * it never silently hands the caller the software tier.
 */

static alp_status_t g_owner_ready = ALP_ERR_NOSUPPORT;

static void require_hw_after(void *fixture)
{
	(void)fixture;
	g_owner_ready = ALP_ERR_NOSUPPORT;
}

ZTEST_SUITE(alp_update_log_require_hw, NULL, NULL, NULL, require_hw_after, NULL);

static alp_status_t owner_ready(void)
{
	return g_owner_ready;
}
static alp_status_t owner_append(const alp_update_log_entry_t *e)
{
	(void)e;
	return ALP_OK;
}
static alp_status_t owner_verify(alp_update_log_verdict_t *v, uint64_t *bad)
{
	(void)bad;
	if (v) *v = ALP_UPDATE_LOG_VERIFY_OK;
	return ALP_OK;
}
static alp_status_t owner_count(uint64_t *out)
{
	if (out) *out = 0;
	return ALP_OK;
}
static alp_status_t owner_get(uint64_t seq, alp_update_log_entry_t *out)
{
	(void)seq;
	(void)out;
	return ALP_ERR_NOT_FOUND;
}
static const alp_update_log_ops_t _owner_ops = {
	.assurance = ALP_UPDATE_LOG_HW_ENFORCED,
	.ready     = owner_ready,
	.append    = owner_append,
	.verify    = owner_verify,
	.count     = owner_count,
	.get       = owner_get,
};
ALP_BACKEND_REGISTER(update_log,
                     require_hw_owner,
                     {
                         .silicon_ref = "*",
                         .vendor      = "require_hw_owner",
                         .base_caps   = 0u,
                         .priority    = 20,
                         .ops         = &_owner_ops,
                         .probe       = NULL,
                     });

/* Owner absent: open() fails closed, no SW fallback. */
ZTEST(alp_update_log_require_hw, test_open_fails_when_owner_absent)
{
	g_owner_ready         = ALP_ERR_NOSUPPORT;
	alp_update_log_t *log = alp_update_log_open();
	zassert_is_null(log);
	zassert_equal(alp_last_error(), ALP_ERR_NOSUPPORT);
}

/* A hard owner fault is still surfaced as itself, not masked as NOSUPPORT. */
ZTEST(alp_update_log_require_hw, test_open_surfaces_owner_hard_error)
{
	g_owner_ready         = ALP_ERR_IO;
	alp_update_log_t *log = alp_update_log_open();
	zassert_is_null(log);
	zassert_equal(alp_last_error(), ALP_ERR_IO);
}

/* Owner answers: open() binds it, and a failed open earlier did not wedge
 * the singleton. */
ZTEST(alp_update_log_require_hw, test_open_succeeds_when_owner_ready)
{
	zassert_is_null(alp_update_log_open());
	g_owner_ready         = ALP_OK;
	alp_update_log_t *log = alp_update_log_open();
	zassert_not_null(log);
	zassert_equal(alp_update_log_assurance(log), ALP_UPDATE_LOG_HW_ENFORCED);
	alp_update_log_close(log);
}
