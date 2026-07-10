/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for the shared errno -> alp_status_t baseline
 * mapper (src/common/alp_errno.h, #630).
 *
 * Scope: the reviewed baseline contract (every case named in the
 * issue's acceptance criteria) plus the explicit per-operation
 * override mechanism, using the exact CAN EAGAIN-as-timeout case as
 * the worked example.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_alp_errno
 *   ctest --test-dir build -R alp_test_alp_errno
 */

#include <errno.h>

#include "common/alp_errno.h"

#include "test_assert.h"

static void test_success_maps_to_ok(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(0), ALP_OK);
}

static void test_invalid_argument(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(EINVAL), ALP_ERR_INVAL);
}

static void test_busy(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(EBUSY), ALP_ERR_BUSY);
	/* Baseline treats EAGAIN as a busy/retry signal (queue-full /
	 * producer-consumer semantics); operations where EAGAIN instead
	 * means "deadline expired" opt into the override mechanism (see
	 * test_override_can_style_eagain_as_timeout below). */
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(EAGAIN), ALP_ERR_BUSY);
}

static void test_timeout(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ETIMEDOUT), ALP_ERR_TIMEOUT);
}

static void test_not_ready_device_not_present(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENOENT), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENODEV), ALP_ERR_NOT_READY);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENXIO), ALP_ERR_NOT_READY);
}

static void test_no_memory(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENOMEM), ALP_ERR_NOMEM);
}

static void test_not_supported_aliases(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENOTSUP), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENOSYS), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(ENOTTY), ALP_ERR_NOSUPPORT);
#if defined(EOPNOTSUPP)
	/* Compiles + maps correctly whether or not EOPNOTSUPP == ENOTSUP on
	 * this libc (acceptance criterion: alias cases compile portably). */
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(EOPNOTSUPP), ALP_ERR_NOSUPPORT);
#endif
}

static void test_unknown_errno_fails_conservatively_as_io(void)
{
	/* EPERM is a real errno, but no case names it -- unknown errors
	 * default to ALP_ERR_IO per the acceptance criteria. */
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno(EPERM), ALP_ERR_IO);
}

/*
 * Override-table contract: an operation-specific table entry wins over
 * the baseline for the errno it names, and every other errno still
 * falls back to the baseline unchanged.  This mirrors the real
 * src/backends/can/yocto_drv.c override (#630): CAN's synchronous
 * socket()/ioctl()/setsockopt() control-plane calls treat EAGAIN as a
 * deadline (ALP_ERR_TIMEOUT), not the baseline's busy/retry
 * (ALP_ERR_BUSY).
 */
static void test_override_can_style_eagain_as_timeout(void)
{
	static const alp_errno_override_t can_overrides[] = {
		{ EAGAIN, ALP_ERR_TIMEOUT },
		{ ENOPROTOOPT, ALP_ERR_NOSUPPORT },
	};
	const size_t n = sizeof(can_overrides) / sizeof(can_overrides[0]);

	/* Overridden errno diverges from the baseline. */
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(EAGAIN, can_overrides, n), ALP_ERR_TIMEOUT);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(ENOPROTOOPT, can_overrides, n),
	                  ALP_ERR_NOSUPPORT);

	/* Everything else still falls back to the shared baseline. */
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(EINVAL, can_overrides, n), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(ENOMEM, can_overrides, n), ALP_ERR_NOMEM);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(0, can_overrides, n), ALP_OK);
}

static void test_override_ex_with_no_table_matches_baseline(void)
{
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(EAGAIN, NULL, 0), ALP_ERR_BUSY);
	ALP_ASSERT_EQ_INT(alp_status_from_posix_errno_ex(EINVAL, NULL, 0), ALP_ERR_INVAL);
}

int main(void)
{
	test_success_maps_to_ok();
	test_invalid_argument();
	test_busy();
	test_timeout();
	test_not_ready_device_not_present();
	test_no_memory();
	test_not_supported_aliases();
	test_unknown_errno_fails_conservatively_as_io();
	test_override_can_style_eagain_as_timeout();
	test_override_ex_with_no_table_matches_baseline();

	ALP_TEST_SUMMARY();
}
