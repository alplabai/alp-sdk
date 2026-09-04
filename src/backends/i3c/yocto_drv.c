/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto i3c_* driver-class backend -- bus PRESENCE check
 * only (issue #1147).
 *
 * Mainline Linux (drivers/i3c/, upstream since v5.0) has NO generic
 * userspace raw-transfer ABI for I3C.  Unlike i2c-dev's
 * ioctl(I2C_RDWR), there is no "/dev/i3c-N" chardev and no uapi
 * header anywhere under include/uapi/linux/i3c/.  Checked directly against a
 * stock kernel headers tree (6.8/6.17): include/linux/i3c/ only ships
 * ccc.h, device.h, master.h -- KERNEL-DRIVER-side headers that pull in
 * <linux/module.h> and cannot be compiled from userspace at all.  The
 * I3C subsystem is bind-only: a kernel i3c_driver claims a target
 * device by matching its Provisioned ID (see
 * Documentation/driver-api/i3c/device-driver-api.rst); userspace never
 * drives the wire directly, the way it can for I2C.
 *
 * This backend can therefore honestly implement exactly one real
 * thing: confirm a bus controller is live under
 * /sys/bus/i3c/devices/i3c-<bus_id> -- the master's own struct device
 * registers there (mirrors the i2c-adapter "i2c-N" convention; see
 * drivers/i3c/master.c i3c_master_register()).  write()/read()/
 * write_read() are ALP_ERR_NOSUPPORT for EVERY bus, always: there is
 * no syscall this backend could issue instead that would actually
 * move a bit on the wire.
 *
 * @par Safety note (#1147 review guidance): because no raw-transfer
 *      path exists at all, nothing here can reach a broadcast CCC or
 *      a dynamic-address-assignment op -- the entire hazard class the
 *      review asked to gate is structurally absent, not merely gated.
 *      If a future kernel ever grows a raw uAPI, that gate (an
 *      explicit opt-in before any broadcast-capable op, mirroring
 *      storage's allow_unsafe_write) must land in the SAME change
 *      that turns write()/read() real -- never real-and-ungated in
 *      one step.
 *
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds
 * where this TU compiles to an empty object.  Selected on any silicon
 * (silicon_ref "*") because the i3c-core sysfs ABI is SoC-agnostic --
 * the device tree decides which physical controller backs each
 * i3c-N bus.
 *
 * @par Status: PARTIAL real implementation -- open()/close() only, a
 *      presence check.  write()/read()/write_read() are an honest
 *      NOSUPPORT, not a stand-in real implementation.
 *      BENCH-UNVERIFIED: no /sys/bus/i3c tree exists in this build
 *      environment, and the "i3c-<N>" master naming convention this
 *      file relies on has not been confirmed against a running V2N
 *      kernel.  Do not read anything in this file as silicon-proven.
 */

#if defined(__linux__)

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/stat.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/i3c.h>
#include <alp/peripheral.h>

#include "i3c_ops.h"
#include "common/alp_errno.h"

#define Y_I3C_SYSFS_ROOT "/sys/bus/i3c/devices"
/* "/sys/bus/i3c/devices/i3c-" + up to 10 digits + NUL. */
#define Y_I3C_PATH_MAX 64

/* Test-only seam: redirects _bus_present()'s sysfs root to a fixture
 * directory instead of the real /sys/bus/i3c/devices tree.  Default
 * NULL means "use the real path" -- same pattern as the USB backend's
 * g_usb_test_sysfs_root_hook. */
static const char *g_i3c_test_sysfs_root_hook = NULL;

static const char *_sysfs_root(void)
{
	return (g_i3c_test_sysfs_root_hook != NULL) ? g_i3c_test_sysfs_root_hook : Y_I3C_SYSFS_ROOT;
}

/**
 * @brief True iff <sysfs_root>/i3c-<bus_id> exists as a directory.
 *
 * Builds the path with snprintf() and rejects a truncated result
 * outright (returns false) rather than stat()-ing a chopped path that
 * could coincidentally name something else.
 */
static bool _bus_present(uint32_t bus_id)
{
	char path[Y_I3C_PATH_MAX];
	int  n = snprintf(path, sizeof(path), "%s/i3c-%u", _sysfs_root(), (unsigned)bus_id);
	if (n < 0 || (size_t)n >= sizeof(path)) return false;

	struct stat st;
	if (stat(path, &st) != 0) return false;
	return S_ISDIR(st.st_mode);
}

/**
 * @brief Presence check only -- see the file header.  Does not open
 *        any fd (there is nothing to open: no raw-transfer chardev
 *        exists), so close() has nothing to release either.
 */
static alp_status_t
y_open(const alp_i3c_config_t *cfg, alp_i3c_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg == NULL || st == NULL || caps_out == NULL) return ALP_ERR_INVAL;
	if (!_bus_present(cfg->bus_id)) return ALP_ERR_NOT_READY;

	st->dev     = NULL;
	st->bus_id  = cfg->bus_id;
	st->be_data = NULL;
	return ALP_OK;
}

/** @brief NOSUPPORT -- see file header: no userspace raw-transfer ABI exists. */
static alp_status_t
y_write(alp_i3c_backend_state_t *st, uint8_t addr, const uint8_t *data, size_t len)
{
	(void)st;
	(void)addr;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT -- see file header: no userspace raw-transfer ABI exists. */
static alp_status_t y_read(alp_i3c_backend_state_t *st, uint8_t addr, uint8_t *data, size_t len)
{
	(void)st;
	(void)addr;
	(void)data;
	(void)len;
	return ALP_ERR_NOSUPPORT;
}

/** @brief NOSUPPORT -- see file header: no userspace raw-transfer ABI exists. */
static alp_status_t y_write_read(alp_i3c_backend_state_t *st,
                                 uint8_t                  addr,
                                 const uint8_t           *wdata,
                                 size_t                   wlen,
                                 uint8_t                 *rdata,
                                 size_t                   rlen)
{
	(void)st;
	(void)addr;
	(void)wdata;
	(void)wlen;
	(void)rdata;
	(void)rlen;
	return ALP_ERR_NOSUPPORT;
}

static const alp_i3c_ops_t _ops = {
	.open       = y_open,
	.write      = y_write,
	.read       = y_read,
	.write_read = y_write_read,
	.close      = NULL, /* nothing was opened -- see y_open() */
};

ALP_BACKEND_REGISTER(i3c,
                     yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
