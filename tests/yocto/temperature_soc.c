/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Plain-CMake tests for alp_temperature_read_die_milli_c()
 * (src/yocto/temperature_yocto.c) against a fake sysfs thermal tree.
 *
 * The backend is compiled into this binary with ALP_THERMAL_SYSFS_ROOT
 * pointing at a scratch directory under /tmp, so no real
 * /sys/class/thermal is touched.  Covered: zones picked by `type`
 * prefix (not index), hottest wins, non-SoC zones ignored, index holes
 * skipped, NOSUPPORT when no zone matches, IO when every matched zone's
 * temp is unreadable, negative temperatures.
 *
 * Build with:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_temperature_soc
 *   ctest --test-dir build -R alp_test_temperature_soc
 */

#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <alp/peripheral.h>
#include <alp/temperature.h>

#include "test_assert.h"

#define ROOT      ALP_THERMAL_SYSFS_ROOT
#define MAX_ZONES 32

static void clear_tree(void)
{
	char path[128];
	for (int i = 0; i < MAX_ZONES; i++) {
		snprintf(path, sizeof path, ROOT "/thermal_zone%d/type", i);
		remove(path);
		snprintf(path, sizeof path, ROOT "/thermal_zone%d/temp", i);
		remove(path);
		snprintf(path, sizeof path, ROOT "/thermal_zone%d", i);
		rmdir(path);
	}
}

/* temp == NULL leaves the temp file absent (unreadable zone). */
static void add_zone(int idx, const char *type, const char *temp)
{
	char  path[128];
	FILE *f;

	mkdir(ROOT, 0755);
	snprintf(path, sizeof path, ROOT "/thermal_zone%d", idx);
	mkdir(path, 0755);
	snprintf(path, sizeof path, ROOT "/thermal_zone%d/type", idx);
	f = fopen(path, "w");
	if (f != NULL) {
		fprintf(f, "%s\n", type);
		fclose(f);
	}
	if (temp != NULL) {
		snprintf(path, sizeof path, ROOT "/thermal_zone%d/temp", idx);
		f = fopen(path, "w");
		if (f != NULL) {
			fprintf(f, "%s\n", temp);
			fclose(f);
		}
	}
}

static void test_null_arg(void)
{
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(NULL), ALP_ERR_INVAL);
}

static void test_hottest_cpu_zone_wins_and_acpitz_ignored(void)
{
	int32_t v = 0;
	clear_tree();
	add_zone(0, "cpu-thermal0", "51000");
	add_zone(1, "cpu-thermal1", "58250");
	add_zone(2, "acpitz", "99000");
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(&v), ALP_OK);
	ALP_ASSERT_EQ_INT(v, 58250);
}

static void test_selection_is_by_type_not_index(void)
{
	int32_t v = 0;
	clear_tree();
	add_zone(0, "acpitz", "99000");
	add_zone(3, "cpu-thermal0", "47000"); /* hole at 1, 2 */
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(&v), ALP_OK);
	ALP_ASSERT_EQ_INT(v, 47000);
}

static void test_negative_temperature(void)
{
	int32_t v = 0;
	clear_tree();
	add_zone(0, "cpu-thermal0", "-12500");
	add_zone(1, "cpu-thermal1", "-20000");
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(&v), ALP_OK);
	ALP_ASSERT_EQ_INT(v, -12500);
}

static void test_unreadable_zone_skipped_when_another_reads(void)
{
	int32_t v = 0;
	clear_tree();
	add_zone(0, "cpu-thermal0", NULL);
	add_zone(1, "cpu-thermal1", "40000");
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(&v), ALP_OK);
	ALP_ASSERT_EQ_INT(v, 40000);
}

static void test_no_matching_zone_is_nosupport(void)
{
	int32_t v = 1234;
	clear_tree();
	add_zone(0, "acpitz", "99000");
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(&v), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(v, 1234); /* untouched on failure */
}

static void test_all_matched_unreadable_is_io(void)
{
	int32_t v = 1234;
	clear_tree();
	add_zone(0, "cpu-thermal0", NULL);
	add_zone(1, "cpu-thermal1", NULL);
	ALP_ASSERT_EQ_INT(alp_temperature_read_die_milli_c(&v), ALP_ERR_IO);
	ALP_ASSERT_EQ_INT(v, 1234);
}

static void test_on_module_read_is_nosupport(void)
{
	int32_t v = 0;
	ALP_ASSERT_EQ_INT(alp_temperature_read_milli_c(&v), ALP_ERR_NOSUPPORT);
}

int main(void)
{
	test_null_arg();
	test_hottest_cpu_zone_wins_and_acpitz_ignored();
	test_selection_is_by_type_not_index();
	test_negative_temperature();
	test_unreadable_zone_skipped_when_another_reads();
	test_no_matching_zone_is_nosupport();
	test_all_matched_unreadable_is_io();
	test_on_module_read_is_nosupport();
	clear_tree();
	rmdir(ROOT);

	ALP_TEST_SUMMARY();
}
