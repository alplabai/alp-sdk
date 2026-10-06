/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2352: the Yocto CAN backend must resolve an E1M bus id to the
 * Linux netdev published in the devicetree property
 * `alp,e1m-can-netdev` (a NUL-separated string list indexed by bus id),
 * and fall back to "can<bus_id>" when the property is absent or has no
 * usable entry for that bus.  #includes yocto_drv.c (no socket needed).
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_can_yocto_netdev_map
 *   ctest --test-dir build -R alp_test_can_yocto_netdev_map
 */

#include <stdio.h>
#include <string.h>

#include "test_assert.h"

#include "../../src/backends/can/yocto_drv.c"

void alp_can_close_finalize(void *owner)
{
	(void)owner;
}

static void name_via_file(const char *bytes, size_t n, unsigned bus, char *out, size_t cap)
{
	const char *path = "alp_can_netdev_map.tmp";
	FILE       *f    = fopen(path, "wb");
	ALP_ASSERT_TRUE(f != NULL);
	fwrite(bytes, 1, n, f);
	fclose(f);
	g_can_test_netdev_map_path = path;
	y_can_netdev_name(bus, out, cap);
	remove(path);
}

int main(void)
{
	char out[IFNAMSIZ];

	/* The pure parser: E1M CAN0 -> can1, CAN1 -> can0 (V2N). */
	static const char swapped[] = "can1\0can0"; /* + implicit NUL */
	ALP_ASSERT_EQ_INT(y_can_netdev_from_map(swapped, sizeof(swapped), 0, out, sizeof(out)), 0);
	ALP_ASSERT_TRUE(strcmp(out, "can1") == 0);
	ALP_ASSERT_EQ_INT(y_can_netdev_from_map(swapped, sizeof(swapped), 1, out, sizeof(out)), 0);
	ALP_ASSERT_TRUE(strcmp(out, "can0") == 0);
	/* Out of range, empty entry, and an unterminated tail are all "no entry". */
	ALP_ASSERT_EQ_INT(y_can_netdev_from_map(swapped, sizeof(swapped), 2, out, sizeof(out)), -1);
	static const char holey[] = "\0can0";
	ALP_ASSERT_EQ_INT(y_can_netdev_from_map(holey, sizeof(holey), 0, out, sizeof(out)), -1);
	ALP_ASSERT_EQ_INT(y_can_netdev_from_map(holey, sizeof(holey), 1, out, sizeof(out)), 0);
	ALP_ASSERT_EQ_INT(y_can_netdev_from_map("can0", 4, 0, out, sizeof(out)), -1);

	/* End to end through the file read. */
	name_via_file(swapped, sizeof(swapped), 0, out, sizeof(out));
	ALP_ASSERT_TRUE(strcmp(out, "can1") == 0);
	name_via_file(holey, sizeof(holey), 0, out, sizeof(out)); /* empty -> fallback */
	ALP_ASSERT_TRUE(strcmp(out, "can0") == 0);
	name_via_file(swapped, sizeof(swapped), 3, out, sizeof(out)); /* beyond map -> fallback */
	ALP_ASSERT_TRUE(strcmp(out, "can3") == 0);

	/* No property at all (non-V2N SoMs): literal can<bus_id>. */
	g_can_test_netdev_map_path = "alp_can_netdev_map.does-not-exist";
	y_can_netdev_name(1, out, sizeof(out));
	ALP_ASSERT_TRUE(strcmp(out, "can1") == 0);
	ALP_TEST_SUMMARY();
}
