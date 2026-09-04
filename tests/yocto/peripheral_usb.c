/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression coverage for the #1141 review blockers on the Yocto USB
 * backend (src/backends/usb/yocto_drv.c):
 *
 *   - blockers 4+5: alp_usb_host_enable()/alp_usb_host_disable() must
 *     be honest ALP_ERR_NOSUPPORT, never a faked ALP_OK -- an earlier
 *     version wrote authorized_default and claimed success even
 *     though the controller kept running / stayed enumerated.
 *   - device (gadget) role stays ALP_ERR_NOSUPPORT (no UDC/configfs
 *     path this backend can create).
 *   - minor: _is_roothub_name() must reject an ERANGE overflow rather
 *     than trust an unchecked strtoul().
 *   - minor: _discover_buses() must report an error (not a
 *     silently-truncated ALP_OK) when more root hubs exist than the
 *     fixed-capacity bus array can track.
 *
 * This file #includes the real backend .c file directly (same
 * technique as tests/yocto/peripheral_wdt.c) to reach its file-local
 * y_host_enable()/y_host_disable()/y_dev_open()/_is_roothub_name()/
 * _discover_buses() and the g_usb_test_sysfs_root_hook seam, which
 * points _discover_buses() at a real temporary directory instead of
 * the real /sys/bus/usb/devices tree.
 *
 * Build + run:
 *   cmake -B build -DALP_OS=yocto -DALP_BUILD_TESTS=ON
 *   cmake --build build --target alp_test_peripheral_usb
 *   ctest --test-dir build -R alp_test_peripheral_usb
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "test_assert.h"

#include "../../src/backends/usb/yocto_drv.c"

/* ------------------------------------------------------------------ */
/* Fixture: a real temp directory standing in for /sys/bus/usb/devices */
/* ------------------------------------------------------------------ */

static char g_dir_tmpl[] = "/tmp/alp_test_usb_sysfs_XXXXXX";

static void make_entry(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	int fd = open(path, O_CREAT | O_WRONLY, 0600);
	if (fd >= 0) close(fd);
}

static void remove_entry(const char *dir, const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	unlink(path);
}

/* ------------------------------------------------------------------ */

static void test_is_roothub_name(void)
{
	unsigned bus = 999;
	ALP_ASSERT_TRUE(_is_roothub_name("usb1", &bus));
	ALP_ASSERT_EQ_INT(bus, 1);

	ALP_ASSERT_TRUE(_is_roothub_name("usb12", &bus));
	ALP_ASSERT_EQ_INT(bus, 12);

	ALP_ASSERT_TRUE(!_is_roothub_name("1-1", &bus));      /* a device, not a root hub */
	ALP_ASSERT_TRUE(!_is_roothub_name("usb1:1.0", &bus)); /* an interface node */
	ALP_ASSERT_TRUE(!_is_roothub_name("usb", &bus));      /* no digits at all */
	ALP_ASSERT_TRUE(!_is_roothub_name("usbA", &bus));     /* not a digit */
	ALP_ASSERT_TRUE(!_is_roothub_name("usb1a", &bus));    /* trailing junk */
}

/* Minor fix: an unchecked strtoul() would accept an absurd bus number
 * (or read undefined ERANGE-saturated behaviour) instead of rejecting
 * outright. */
static void test_is_roothub_name_rejects_erange(void)
{
	unsigned bus = 999;
	ALP_ASSERT_TRUE(!_is_roothub_name("usb999999999999999999999999999999", &bus));
}

/* Blockers 4+5: these must never return ALP_OK -- see the file-header
 * note on why a real controller start/stop cannot be implemented
 * faithfully through authorized_default. */
static void test_host_enable_disable_are_honest_nosupport(void)
{
	y_usb_host_data_t    d  = { .n_buses = 0 };
	alp_usb_host_state_t st = { .be_data = &d };

	ALP_ASSERT_EQ_INT(y_host_enable(&st), ALP_ERR_NOSUPPORT);
	ALP_ASSERT_EQ_INT(y_host_disable(&st), ALP_ERR_NOSUPPORT);
}

static void test_dev_open_is_nosupport(void)
{
	alp_usb_device_config_t cfg  = { 0 };
	alp_usb_dev_state_t     st   = { 0 };
	alp_capabilities_t      caps = { 0 };

	ALP_ASSERT_EQ_INT(y_dev_open(&cfg, &st, &caps), ALP_ERR_NOSUPPORT);
}

static void test_discover_buses_finds_only_bare_roothub_names(void)
{
	char dir[sizeof(g_dir_tmpl)];
	memcpy(dir, g_dir_tmpl, sizeof(g_dir_tmpl));
	ALP_ASSERT_TRUE(mkdtemp(dir) != NULL);

	make_entry(dir, "usb1");
	make_entry(dir, "usb2");
	make_entry(dir, "1-1");      /* a real device under usb1, not a root hub */
	make_entry(dir, "usb1:1.0"); /* a real interface node, not a root hub */

	g_usb_test_sysfs_root_hook = dir;
	y_usb_host_data_t d;
	alp_status_t      rc       = _discover_buses(&d);
	g_usb_test_sysfs_root_hook = NULL;

	ALP_ASSERT_EQ_INT(rc, ALP_OK);
	ALP_ASSERT_EQ_INT((int)d.n_buses, 2);

	remove_entry(dir, "usb1");
	remove_entry(dir, "usb2");
	remove_entry(dir, "1-1");
	remove_entry(dir, "usb1:1.0");
	rmdir(dir);
}

static void test_discover_buses_empty_dir_is_nosupport(void)
{
	char dir[sizeof(g_dir_tmpl)];
	memcpy(dir, g_dir_tmpl, sizeof(g_dir_tmpl));
	ALP_ASSERT_TRUE(mkdtemp(dir) != NULL);

	g_usb_test_sysfs_root_hook = dir;
	y_usb_host_data_t d;
	alp_status_t      rc       = _discover_buses(&d);
	g_usb_test_sysfs_root_hook = NULL;

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOSUPPORT);

	rmdir(dir);
}

/* Minor fix: more root hubs than Y_USB_MAX_BUSES must surface as an
 * error, not a silently-truncated ALP_OK over a partial bus list. */
static void test_discover_buses_overflow_is_nomem(void)
{
	char dir[sizeof(g_dir_tmpl)];
	memcpy(dir, g_dir_tmpl, sizeof(g_dir_tmpl));
	ALP_ASSERT_TRUE(mkdtemp(dir) != NULL);

	char names[Y_USB_MAX_BUSES + 1u][16];
	for (unsigned i = 0; i <= Y_USB_MAX_BUSES; ++i) {
		snprintf(names[i], sizeof(names[i]), "usb%u", i + 1u);
		make_entry(dir, names[i]);
	}

	g_usb_test_sysfs_root_hook = dir;
	y_usb_host_data_t d;
	alp_status_t      rc       = _discover_buses(&d);
	g_usb_test_sysfs_root_hook = NULL;

	ALP_ASSERT_EQ_INT(rc, ALP_ERR_NOMEM);

	for (unsigned i = 0; i <= Y_USB_MAX_BUSES; ++i) {
		remove_entry(dir, names[i]);
	}
	rmdir(dir);
}

int main(void)
{
	test_is_roothub_name();
	test_is_roothub_name_rejects_erange();
	test_host_enable_disable_are_honest_nosupport();
	test_dev_open_is_nosupport();
	test_discover_buses_finds_only_bare_roothub_names();
	test_discover_buses_empty_dir_is_nosupport();
	test_discover_buses_overflow_is_nomem();

	ALP_TEST_SUMMARY();
}
