/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tests for the two errno -> alp_status_t baselines in
 * src/common/alp_errno.h (issue #1638).
 *
 * The two domains differ by SIGN: Zephyr driver APIs return `-EXXX`, the
 * POSIX baseline takes `+EXXX`.  27 Zephyr backends hand-rolled their own
 * switch because the negative-domain twin did not exist, and they drifted
 * -- 16 answered ALP_ERR_TIMEOUT for -EAGAIN, 11 fell through to
 * ALP_ERR_IO.  These tests pin the twin's answers so the 27 can delegate.
 */

#include <errno.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "alp_errno.h"

ZTEST_SUITE(alp_errno_mapping, NULL, NULL, NULL, NULL, NULL);

ZTEST(alp_errno_mapping, test_zephyr_baseline_arms)
{
	zassert_equal(alp_status_from_zephyr_errno(0), ALP_OK);
	zassert_equal(alp_status_from_zephyr_errno(-EINVAL), ALP_ERR_INVAL);
	zassert_equal(alp_status_from_zephyr_errno(-EBUSY), ALP_ERR_BUSY);
	zassert_equal(alp_status_from_zephyr_errno(-ETIMEDOUT), ALP_ERR_TIMEOUT);
	zassert_equal(alp_status_from_zephyr_errno(-ENOMEM), ALP_ERR_NOMEM);
	zassert_equal(alp_status_from_zephyr_errno(-ENODEV), ALP_ERR_NOT_READY);
	zassert_equal(alp_status_from_zephyr_errno(-ENOENT), ALP_ERR_NOT_READY);
	zassert_equal(alp_status_from_zephyr_errno(-ENXIO), ALP_ERR_NOT_READY);
	zassert_equal(alp_status_from_zephyr_errno(-ENOTSUP), ALP_ERR_NOSUPPORT);
	zassert_equal(alp_status_from_zephyr_errno(-ENOSYS), ALP_ERR_NOSUPPORT);
	zassert_equal(alp_status_from_zephyr_errno(-ENOTTY), ALP_ERR_NOSUPPORT);
	zassert_equal(alp_status_from_zephyr_errno(-EIO), ALP_ERR_IO);
	zassert_equal(alp_status_from_zephyr_errno(-EPIPE), ALP_ERR_IO, "unmapped falls to IO");
}

/* The two arms the POSIX baseline has no case for at all.  Both appear in
 * the storage and JPEG mappers being migrated, so the twin has to carry
 * them locally or delegation would silently demote them to ALP_ERR_IO. */
ZTEST(alp_errno_mapping, test_zephyr_local_arms_the_posix_baseline_lacks)
{
	zassert_equal(alp_status_from_zephyr_errno(-ENOSPC),
	              ALP_ERR_NOMEM,
	              "storage/zephyr_littlefs.c and jpeg/alif_hantro.c both rely on this");
	zassert_equal(alp_status_from_zephyr_errno(-ERANGE),
	              ALP_ERR_OUT_OF_RANGE,
	              "storage/zephyr_littlefs.c and storage/zephyr_flash.c both rely on this");

	/* Proof that these really are local: the POSIX baseline maps neither. */
	zassert_equal(alp_status_from_posix_errno(ENOSPC), ALP_ERR_IO);
	zassert_equal(alp_status_from_posix_errno(ERANGE), ALP_ERR_IO);
}

/* The deliberate divergence from the POSIX baseline.  If this test is ever
 * "fixed" to agree with alp_status_from_posix_errno(), read the rationale in
 * alp_errno.h first -- 16 Zephyr backends depend on this answer. */
ZTEST(alp_errno_mapping, test_zephyr_eagain_is_timeout_not_busy)
{
	zassert_equal(alp_status_from_zephyr_errno(-EAGAIN), ALP_ERR_TIMEOUT);
	zassert_equal(alp_status_from_posix_errno(EAGAIN),
	              ALP_ERR_BUSY,
	              "the POSIX baseline must be left alone by this change");
}

ZTEST(alp_errno_mapping, test_zephyr_rejects_positive_errno)
{
	/* The whole reason the two functions exist separately is that the
	 * domains differ by sign.  A positive value handed to the Zephyr
	 * mapper is a caller bug and must land on ALP_ERR_IO rather than
	 * accidentally matching a case arm. */
	zassert_equal(alp_status_from_zephyr_errno(EINVAL), ALP_ERR_IO);
	zassert_equal(alp_status_from_zephyr_errno(EAGAIN), ALP_ERR_IO);
	zassert_equal(alp_status_from_zephyr_errno(ENOSPC), ALP_ERR_IO);
}

ZTEST(alp_errno_mapping, test_zephyr_override_form)
{
	/* Overrides are matched on the NEGATIVE value, unlike the POSIX
	 * table's positive keys -- each _ex form matches in its own domain.
	 * A table keyed positively here would silently never match. */
	static const alp_errno_override_t ov[] = {
		{ -ENOSPC, ALP_ERR_OUT_OF_RANGE },
	};
	zassert_equal(alp_status_from_zephyr_errno_ex(-ENOSPC, ov, ARRAY_SIZE(ov)),
	              ALP_ERR_OUT_OF_RANGE,
	              "an override must win over the baseline");
	zassert_equal(alp_status_from_zephyr_errno_ex(-EBUSY, ov, ARRAY_SIZE(ov)),
	              ALP_ERR_BUSY,
	              "a non-overridden arm must fall through to the baseline");
	zassert_equal(alp_status_from_zephyr_errno_ex(-EBUSY, NULL, 0),
	              ALP_ERR_BUSY,
	              "an empty table is the plain baseline");
}
