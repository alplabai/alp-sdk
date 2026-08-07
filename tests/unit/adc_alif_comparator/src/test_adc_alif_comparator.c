/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * #1120: adc_alif.c's adc_start_read() unconditionally dereferenced
 * `sequence->options->user_data`, even though Zephyr's adc_sequence contract
 * makes `options` optional (NULL means an ordinary one-shot read). A NULL
 * options with the old code would fault the driver before completing the
 * conversion. Exercised here against the exact NULL-safe guard the driver
 * now calls, on the host, with no MMIO/devicetree involved.
 */
#include <zephyr/drivers/adc.h>
#include <zephyr/ztest.h>

#include "adc_alif_comparator.h"

ZTEST_SUITE(adc_alif_comparator, NULL, NULL, NULL, NULL, NULL);

/* The repro from the issue: a valid single-channel one-shot adc_sequence
 * with options == NULL must not be dereferenced.
 */
ZTEST(adc_alif_comparator, test_null_options_is_ordinary_one_shot)
{
	struct adc_sequence seq = {
		.options = NULL,
	};

	zassert_is_null(adc_alif_sequence_comparator(&seq),
	                "NULL options must yield a NULL comparator, not a crash");
}

/* Repeated/extra-sampling reads that DO supply options must still surface
 * the caller's user_data pointer -- the guard must not degrade the non-NULL
 * path.
 */
ZTEST(adc_alif_comparator, test_non_null_options_yields_user_data)
{
	int                         sentinel;
	struct adc_sequence_options opts = {
		.user_data       = &sentinel,
		.extra_samplings = 3,
	};
	struct adc_sequence seq = {
		.options = &opts,
	};

	zassert_equal_ptr(adc_alif_sequence_comparator(&seq),
	                  &sentinel,
	                  "non-NULL options must surface options->user_data unchanged");
}

/* options supplied but user_data left NULL (a caller not using the
 * comparator feature) must also come through as NULL, not a stale pointer.
 */
ZTEST(adc_alif_comparator, test_non_null_options_null_user_data)
{
	struct adc_sequence_options opts = {
		.user_data = NULL,
	};
	struct adc_sequence seq = {
		.options = &opts,
	};

	zassert_is_null(adc_alif_sequence_comparator(&seq),
	                "options->user_data == NULL must pass through as NULL");
}
