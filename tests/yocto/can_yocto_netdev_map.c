/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Issue #2352: the Yocto CAN backend must resolve an E1M bus id to the
 * Linux netdev published in the devicetree property
 * `alp,e1m-can-netdev` (a NUL-separated string list indexed by bus id).
 * It falls back to "can<bus_id>" ONLY when the property is absent; a
 * present property with no usable entry for the bus is a failure
 * (falling back would reopen the swapped port).  #includes yocto_drv.c
 * (no socket needed); CMake points ALP_CAN_NETDEV_MAP_PATH at a file in
 * the build dir, which this test rewrites per case.
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

static void put_map(const char *bytes, size_t n)
{
	FILE *f = fopen(ALP_CAN_NETDEV_MAP_PATH, "wb");
	ALP_ASSERT_TRUE(f != NULL);
	if (f == NULL) return;
	fwrite(bytes, 1, n, f);
	fclose(f);
}

static alp_status_t resolve(unsigned bus, char *out, size_t cap)
{
	return y_can_netdev_name(bus, out, cap);
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

	/* Property present: it is authoritative. */
	put_map(swapped, sizeof(swapped));
	ALP_ASSERT_EQ_INT(resolve(0, out, sizeof(out)), ALP_OK);
	ALP_ASSERT_TRUE(strcmp(out, "can1") == 0);
	ALP_ASSERT_EQ_INT(resolve(1, out, sizeof(out)), ALP_OK);
	ALP_ASSERT_TRUE(strcmp(out, "can0") == 0);
	/* empty entry and a bus beyond the map: failure, NOT can<bus_id>. */
	put_map(holey, sizeof(holey));
	ALP_ASSERT_EQ_INT(resolve(0, out, sizeof(out)), ALP_ERR_NOT_READY);
	put_map(swapped, sizeof(swapped));
	ALP_ASSERT_EQ_INT(resolve(3, out, sizeof(out)), ALP_ERR_NOT_READY);
	/* a map longer than the read buffer is rejected, not silently truncated. */
	char big[200];
	memset(big, 'x', sizeof(big));
	put_map(big, sizeof(big));
	ALP_ASSERT_EQ_INT(resolve(0, out, sizeof(out)), ALP_ERR_NOT_READY);

	/* Truncation edge: exactly the buffer size (64) is rejected as "maybe
	 * truncated" (the read cannot prove it hit EOF). */
	char edge[64];
	memset(edge, 'x', sizeof(edge));
	put_map(edge, sizeof(edge));
	ALP_ASSERT_EQ_INT(resolve(0, out, sizeof(out)), ALP_ERR_NOT_READY);

	/* Unreadable path (not ENOENT): the map "file" has a regular file as a
	 * directory component -> ENOTDIR.  Must fail closed, NOT fall back. */
	put_map(swapped, sizeof(swapped));
	char bad_path[512];
	snprintf(bad_path, sizeof(bad_path), "%s/x", ALP_CAN_NETDEV_MAP_PATH);
	ALP_ASSERT_EQ_INT(y_can_netdev_name_at(bad_path, 0, out, sizeof(out)), ALP_ERR_NOT_READY);

	/* Property absent (non-V2N SoMs): literal can<bus_id>. */
	remove(ALP_CAN_NETDEV_MAP_PATH);
	ALP_ASSERT_EQ_INT(resolve(1, out, sizeof(out)), ALP_OK);
	ALP_ASSERT_TRUE(strcmp(out, "can1") == 0);
	ALP_ASSERT_EQ_INT(resolve(3, out, sizeof(out)), ALP_OK);
	ALP_ASSERT_TRUE(strcmp(out, "can3") == 0);
	ALP_TEST_SUMMARY();
}
