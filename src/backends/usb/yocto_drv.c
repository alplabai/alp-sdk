/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto usb_* driver-class backend -- HOST *presence
 * check* only (issue #1141).  alp_usb_host_open() enumerates root
 * hubs under /sys/bus/usb/devices/usbN/ (Documentation/ABI/testing/
 * sysfs-bus-usb) to answer "is a USB host stack live under this
 * kernel", matching the documented ALP_ERR_NOSUPPORT ("no host stack
 * wired") open() failure case.
 *
 * alp_usb_host_enable() / alp_usb_host_disable() are ALP_ERR_NOSUPPORT
 * -- see y_host_enable() / y_host_disable()'s own doxygen for why:
 * the only real sysfs knob available (authorized_default) does not
 * start or stop a controller, so writing to it and returning ALP_OK
 * would be a FAKED success -- worse than an honest NOSUPPORT.  An
 * earlier version of this file did exactly that; #1141 review caught
 * it (blockers 4+5): host_disable() left the controller running with
 * every already-enumerated device still authorized (nothing detaches
 * -- authorized_default only gates NEWLY attached devices), and
 * host_enable() was normally a literal no-op (authorized_default
 * already defaults to 1) that also could not re-authorize a device
 * that attached while deauthorized, so enable-after-disable was not
 * even the inverse of disable.
 *
 * No libusb, no usbfs claim/transfer: <alp/usb.h>'s host surface is
 * intentionally minimal (open/enable/disable/close only -- no device
 * enumeration or transfer calls are declared), so sysfs discovery
 * alone is enough for the one op (open) this backend actually
 * implements for real.
 *
 * @par Device (gadget) role: NOT implemented here -- see y_dev_open()
 *      below for why.
 *
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds
 * where this TU compiles to an empty object.  Selected on any
 * silicon (silicon_ref "*") because the USB core sysfs ABI is
 * SoC-agnostic -- the device tree decides which physical controllers
 * back the enumerated usbN root hubs.
 *
 * @par Status: REAL implementation for host_open()/host_close() (a
 *      presence check only).  host_enable()/host_disable() are an
 *      honest NOSUPPORT, not a stand-in real implementation -- see
 *      above.  BENCH-UNVERIFIED -- no /sys/bus/usb/devices tree with
 *      real root hubs exists in this build environment, so open()
 *      finding zero buses (and therefore returning ALP_ERR_NOSUPPORT)
 *      is the only path exercised here.  Do not read anything in
 *      this file as silicon-proven.
 */

#if defined(__linux__)

#include <dirent.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/usb.h>

#include "usb_ops.h"
#include "common/alp_errno.h"

#define Y_USB_SYSFS_ROOT "/sys/bus/usb/devices"
/* Root hubs rarely number past single digits on an embedded SoM (one
 * per physical controller instance); 16 is generous headroom without
 * an unbounded heap array for a fixed-cardinality kernel fact. */
#define Y_USB_MAX_BUSES 16u

/* Per-host-handle backend data: the USB bus numbers discovered at
 * open() time (one per root hub / physical host controller). */
typedef struct {
	unsigned buses[Y_USB_MAX_BUSES];
	size_t   n_buses;
} y_usb_host_data_t;

/* Test-only seam: overrides the sysfs directory _discover_buses()
 * scans, so a test can point it at a fixture directory instead of the
 * real /sys/bus/usb/devices.  Default NULL means "use the real
 * path". */
static const char *g_usb_test_sysfs_root_hook = NULL;

static const char *_sysfs_root(void)
{
	return (g_usb_test_sysfs_root_hook != NULL) ? g_usb_test_sysfs_root_hook : Y_USB_SYSFS_ROOT;
}

/**
 * @brief True iff @p name is a bare root-hub node name ("usb" + one
 *        or more digits, nothing else -- e.g. "usb1", not "1-1" or
 *        "usb1:1.0").  On match, writes the bus number to @p bus_out.
 */
static bool _is_roothub_name(const char *name, unsigned *bus_out)
{
	if (strncmp(name, "usb", 3) != 0) return false;
	const char *digits = name + 3;
	if (*digits == '\0') return false;
	char *end       = NULL;
	errno           = 0;
	unsigned long v = strtoul(digits, &end, 10);
	if (end == digits || *end != '\0' || errno == ERANGE) return false;
	*bus_out = (unsigned)v;
	return true;
}

/**
 * @brief Enumerate every root hub under the sysfs USB device tree
 *        into @p d.  Returns ALP_ERR_NOSUPPORT if none are found --
 *        no USB host controller is live under this kernel, matching
 *        alp_usb_host_open's documented "no host stack wired" case.
 *
 * Returns ALP_ERR_NOMEM if more than Y_USB_MAX_BUSES root hubs are
 * found -- a fixed-capacity array that silently tracked only the
 * first Y_USB_MAX_BUSES would still report ALP_OK while enable/
 * disable (were they implemented) applied to just a subset of the
 * real controllers; reporting the overflow is the honest option.
 */
static alp_status_t _discover_buses(y_usb_host_data_t *d)
{
	d->n_buses = 0;

	DIR *dir = opendir(_sysfs_root());
	if (dir == NULL) return alp_status_from_posix_errno(errno);

	struct dirent *ent;
	while ((ent = readdir(dir)) != NULL) {
		unsigned bus;
		if (!_is_roothub_name(ent->d_name, &bus)) continue;
		if (d->n_buses >= Y_USB_MAX_BUSES) {
			closedir(dir);
			return ALP_ERR_NOMEM;
		}
		d->buses[d->n_buses++] = bus;
	}
	closedir(dir);

	return (d->n_buses > 0u) ? ALP_OK : ALP_ERR_NOSUPPORT;
}

/**
 * @brief Discover the live root hubs -- a presence check, nothing
 *        more.  See the file header for why enable()/disable() don't
 *        act on the discovered list.
 */
static alp_status_t y_host_open(alp_usb_host_state_t *st, alp_capabilities_t *caps_out)
{
	if (st == NULL || caps_out == NULL) return ALP_ERR_INVAL;

	y_usb_host_data_t *d = (y_usb_host_data_t *)malloc(sizeof(*d));
	if (d == NULL) return ALP_ERR_NOMEM;

	alp_status_t rc = _discover_buses(d);
	if (rc != ALP_OK) {
		free(d);
		return rc;
	}

	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/**
 * @brief NOT implemented -- see the file-header note (#1141 review,
 *        blockers 4+5).  By the time host_open() can find root hubs
 *        under /sys/bus/usb/devices, the kernel has already bound
 *        and started xhci-hcd/ehci-hcd/ohci-hcd; there is no Linux
 *        userspace ABI this backend can drive to "start" a controller
 *        that isn't already running.  Faking ALP_OK here (as a prior
 *        version of this file did, by writing authorized_default's
 *        already-default value) would be worse than this honest
 *        NOSUPPORT.
 */
static alp_status_t y_host_enable(alp_usb_host_state_t *st)
{
	(void)st;
	return ALP_ERR_NOSUPPORT;
}

/**
 * @brief NOT implemented -- see the file-header note (#1141 review,
 *        blockers 4+5).  authorized_default gates only NEWLY attached
 *        devices: writing "0" to it (as a prior version of this file
 *        did) does not stop the controller and does not detach any
 *        device already enumerated, so returning ALP_OK would claim
 *        "stopped" while nothing stopped.  A real implementation
 *        would need controller bind/unbind via
 *        /sys/bus/platform/drivers/<xhci-hcd|ehci-*-hcd|ohci-*-hcd>/
 *        {bind,unbind}, which needs the exact per-SoC platform device
 *        name -- a board-integration decision this generic sysfs
 *        backend does not have enough information to make safely.
 */
static alp_status_t y_host_disable(alp_usb_host_state_t *st)
{
	(void)st;
	return ALP_ERR_NOSUPPORT;
}

static void y_host_close(alp_usb_host_state_t *st)
{
	y_usb_host_data_t *d = (y_usb_host_data_t *)st->be_data;
	if (d != NULL) {
		free(d);
		st->be_data = NULL;
	}
}

/**
 * @brief Device (gadget) role -- deliberately NOT implemented.
 *
 * <alp/usb.h>'s device-role ops (CDC-ACM/MSC/HID) need a UDC bound
 * into a configfs gadget composition -- a system-image concern, not
 * something a chardev/sysfs backend can create on its own (the same
 * reasoning the Wi-Fi/BLE Yocto backends already document in
 * src/yocto/CMakeLists.txt: radio/gadget bring-up is
 * wpa_supplicant/NetworkManager/BlueZ/configfs territory, outside the
 * SDK's reach).  More concretely for #1141: the V2N X-EVK device tree
 * this issue is about (e1m-x-evk.dtsi) enables only the HOST-role
 * controllers -- ehci0/ohci0/hsusb/xhci0 -- with no UDC/gadget node,
 * so there is nothing on this board for a device-role backend to
 * bind to today.  Returning success here would be fabricating a
 * capability that doesn't exist on the hardware this backend targets.
 */
static alp_status_t y_dev_open(const alp_usb_device_config_t *cfg,
                               alp_usb_dev_state_t           *st,
                               alp_capabilities_t            *caps_out)
{
	(void)cfg;
	(void)st;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

static const alp_usb_ops_t _ops = {
	.dev_open    = y_dev_open,
	.dev_enable  = NULL,
	.dev_disable = NULL,
	.dev_write   = NULL,
	.dev_read    = NULL,
	.dev_close   = NULL,

	.host_open    = y_host_open,
	.host_enable  = y_host_enable,
	.host_disable = y_host_disable,
	.host_close   = y_host_close,
};

ALP_BACKEND_REGISTER(usb,
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
