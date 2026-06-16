/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Tiny assert-style harness for plain-CMake tests under tests/yocto/.
 * No external deps; failures print file:line:expected vs actual and
 * exit 1 so CTest marks the test as failed without ambiguity.
 *
 * Counts pass/fail per test binary so a partial pass still gives a
 * useful summary at exit.
 */

#ifndef ALP_TEST_ASSERT_H_
#define ALP_TEST_ASSERT_H_

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int alp_test_pass_count = 0;
static int alp_test_fail_count = 0;

#define ALP_TEST_FAIL(fmt, ...)                                                                    \
	do {                                                                                           \
		fprintf(stderr, "%s:%d: FAIL: " fmt "\n", __FILE__, __LINE__, ##__VA_ARGS__);              \
		++alp_test_fail_count;                                                                     \
	} while (0)

#define ALP_TEST_PASS()                                                                            \
	do {                                                                                           \
		++alp_test_pass_count;                                                                     \
	} while (0)

#define ALP_ASSERT_TRUE(cond)                                                                      \
	do {                                                                                           \
		if (!(cond)) {                                                                             \
			ALP_TEST_FAIL("expected (%s) to be true", #cond);                                      \
		} else {                                                                                   \
			ALP_TEST_PASS();                                                                       \
		}                                                                                          \
	} while (0)

#define ALP_ASSERT_NULL(ptr)                                                                       \
	do {                                                                                           \
		if ((ptr) != NULL) {                                                                       \
			ALP_TEST_FAIL("expected NULL, got non-NULL pointer");                                  \
		} else {                                                                                   \
			ALP_TEST_PASS();                                                                       \
		}                                                                                          \
	} while (0)

#define ALP_ASSERT_EQ_INT(actual, expected)                                                        \
	do {                                                                                           \
		long _a = (long)(actual);                                                                  \
		long _e = (long)(expected);                                                                \
		if (_a != _e) {                                                                            \
			ALP_TEST_FAIL("expected %ld (%s), got %ld", _e, #expected, _a);                        \
		} else {                                                                                   \
			ALP_TEST_PASS();                                                                       \
		}                                                                                          \
	} while (0)

#define ALP_TEST_SUMMARY()                                                                         \
	do {                                                                                           \
		fprintf(stdout, "alp_test: %d passed, %d failed\n", alp_test_pass_count,                   \
		        alp_test_fail_count);                                                              \
		return alp_test_fail_count == 0 ? 0 : 1;                                                   \
	} while (0)

#endif /* ALP_TEST_ASSERT_H_ */
