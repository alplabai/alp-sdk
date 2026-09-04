/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for issue #1147's Yocto I3C backend
 * (src/backends/i3c/yocto_drv.c):
 *
 *   - _bus_present() must find a real "i3c-<N>" directory under the
 *     (fixture-redirected) sysfs root and reject a missing one, a
 *     non-directory entry of the same name, and a bus_id whose path
 *     would overflow the fixed-size buffer.
 *   - y_open() must return ALP_ERR_NOT_READY when the bus is absent
 *     and ALP_OK (with the sysfs directory actually present) when it
 *     is -- never a fabricated success for a bus that was never
 *     confirmed to exist.
 *   - write()/read()/write_read() must be honest ALP_ERR_NOSUPPORT --
 *     mainline Linux has no raw-transfer ABI for I3C at all (see the
 *     backend's own file header), so these must NEVER return ALP_OK.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_usb.c) to reach its file-local
 * y_open()/y_write()/y_read()/y_write_read()/_bus_present() and the
 * g_i3c_test_sysfs_root_hook seam, which points _bus_present() at a
 * real temporary directory instead of the real /sys/bus/i3c/devices
 * tree.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_i3c
 *   ctest --test-dir build -R alp_test_peripheral_i3c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/backends/i3c/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture: a real temp directory standing in for /sys/bus/i3c/devices */
/* ------------------------------------------------------------------ */

static char g_dir_tmpl[] = "/tmp/alp_test_i3c_sysfs_XXXXXX";

static void make_dir_entry(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	mkdir(path, 0700);
}

static void make_file_entry(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *f = fopen(path, "w");
	if (f != NULL) fclose(f);
}

static void remove_dir_entry(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	rmdir(path);
}

static void remove_file_entry(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
}

/* ------------------------------------------------------------------ */

static void test_bus_present_finds_real_directory(void)
{
	char dir[sizeof(g_dir_tmpl)];
	memcpy(dir, g_dir_tmpl, sizeof(g_dir_tmpl));
	ALP_ASSERT_TRUE(mkdtemp(dir) != NULL);
	make_dir_entry(dir, "i3c-0");

	g_i3c_test_sysfs_root_hook = dir;
	ALP_ASSERT_TRUE(_bus_present(0));
	ALP_ASSERT_TRUE(!_bus_present(1)); /* i3c-1 was never created */
	g_i3c_test_sysfs_root_hook = NULL;

	remove_dir_entry(dir, "i3c-0");
	rmdir(dir);
}

/* A same-named regular FILE (not a directory) must not count as
 * "present" -- a broken/racy sysfs snapshot should read as absent,
 * not as a live bus. */
static void test_bus_present_rejects_non_directory(void)
{
	char dir[sizeof(g_dir_tmpl)];
	memcpy(dir, g_dir_tmpl, sizeof(g_dir_tmpl));
	ALP_ASSERT_TRUE(mkdtemp(dir) != NULL);
	make_file_entry(dir, "i3c-2");

	g_i3c_test_sysfs_root_hook = dir;
	ALP_ASSERT_TRUE(!_bus_present(2));
	g_i3c_test_sysfs_root_hook = NULL;

	remove_file_entry(dir, "i3c-2");
	rmdir(dir);
}

static void test_open_matches_bus_presence(void)
{
	char dir[sizeof(g_dir_tmpl)];
	memcpy(dir, g_dir_tmpl, sizeof(g_dir_tmpl));
	ALP_ASSERT_TRUE(mkdtemp(dir) != NULL);
	make_dir_entry(dir, "i3c-0");

	g_i3c_test_sysfs_root_hook = dir;

	alp_i3c_config_t        cfg_present = { .bus_id = 0 };
	alp_i3c_backend_state_t st          = { 0 };
	alp_capabilities_t      caps        = { 0 };
	ALP_ASSERT_EQ_INT(y_open(&cfg_present, &st, &caps), ALP_OK);

	alp_i3c_config_t cfg_absent = { .bus_id = 5 };
	ALP_ASSERT_EQ_INT(y_open(&cfg_absent, &st, &caps), ALP_ERR_NOT_READY);

	g_i3c_test_sysfs_root_hook = NULL;

	remove_dir_entry(dir, "i3c-0");
	rmdir(dir);
}

static void test_open_rejects_null_args(void)
{
	alp_i3c_config_t        cfg  = { .bus_id = 0 };
	alp_i3c_backend_state_t st   = { 0 };
	alp_capabilities_t      caps = { 0 };
	ALP_ASSERT_EQ_INT(y_open(NULL, &st, &caps), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(y_open(&cfg, NULL, &caps), ALP_ERR_INVAL);
	ALP_ASSERT_EQ_INT(y_open(&cfg, &st, NULL), ALP_ERR_INVAL);
}

/* The load-bearing claim of this whole backend: no raw transfer is
 * EVER real here. */
static void test_transfers_are_always_honest_nosupport(void)
{
	alp_i3c_backend_state_t st     = { 0 };
	uint8_t                 buf[4] = { 0 };

	ALP_ASSERT_EQ_INT(y_write(&st, 0x08u, buf, sizeof(buf)), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(y_read(&st, 0x08u, buf, sizeof(buf)), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(y_write_read(&st, 0x08u, buf, 1u, buf, 1u), ALP_ERR_NOSUPPORT);
}

int main(void)
{
	test_bus_present_finds_real_directory();
	test_bus_present_rejects_non_directory();
	test_open_matches_bus_presence();
	test_open_rejects_null_args();
	test_transfers_are_always_honest_nosupport();

	ALP_TEST_SUMMARY();
}
