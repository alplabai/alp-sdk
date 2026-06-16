/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto wdt_* driver-class backend.  Binds the alp_wdt
 * dispatcher's ops vtable to the kernel watchdog character device
 * (/dev/watchdogN) via the WDIOC_* ioctls from <linux/watchdog.h>.
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) wins on non-Linux native_sim builds where
 * this TU compiles to an empty object.
 *
 * Selected on any silicon (silicon_ref "*"): the kernel watchdog ABI
 * is SoC-agnostic and the device-tree / kernel driver decides which
 * physical watchdog backs /dev/watchdogN and what its reset action is.
 *
 * On the alp_wdt_action_t mapping: the Linux watchdog ABI exposes NO
 * knob for reset-scope (SoC vs core) or interrupt-only mode -- that is
 * fixed by the kernel driver + device-tree.  cfg.on_timeout is
 * therefore informational only on Linux; we honour timeout_ms (the one
 * field the ABI lets us set) and leave the action to the platform.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <linux/watchdog.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/wdt.h>

#include "wdt_ops.h"

/* Per-handle backend data: the open watchdog chardev fd plus whether
 * the driver advertised magic-close support (so close() can disarm
 * via the 'V' write when the platform honours it). */
typedef struct {
	int  fd;
	bool magic_close;
} y_wdt_data_t;

/** @brief Map a (positive) errno value to the closest alp_status_t. */
static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case EINVAL:
		return ALP_ERR_INVAL;
	case EBUSY:
		return ALP_ERR_BUSY;
	case ENOTTY:
	case ENOSYS:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

/**
 * @brief Open /dev/watchdog<wdt_id>, program the timeout, read caps.
 *
 * Opening the node arms the watchdog immediately (Linux semantics).
 * The alp config carries timeout_ms; WDIOC_SETTIMEOUT works in whole
 * seconds, so we round up to at least 1 s.  WDIOC_GETSUPPORT tells us
 * whether magic-close is available for the close()-disarm path.
 */
static alp_status_t y_open(uint32_t wdt_id, const alp_wdt_config_t *cfg,
                           alp_wdt_backend_state_t *st, alp_capabilities_t *caps_out)
{
	char path[32];
	int  n = snprintf(path, sizeof(path), "/dev/watchdog%u", (unsigned)wdt_id);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return _errno_to_alp(errno);

	y_wdt_data_t *d = (y_wdt_data_t *)malloc(sizeof(*d));
	if (d == NULL) {
		close(fd);
		return ALP_ERR_NOMEM;
	}
	d->fd          = fd;
	d->magic_close = false;

	/* Round timeout_ms up to whole seconds (min 1 s); the dispatcher
     * already rejected a zero timeout_ms before reaching the backend. */
	int timeout_s = (int)((cfg->timeout_ms + 999u) / 1000u);
	if (timeout_s < 1) timeout_s = 1;
	if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout_s) < 0) {
		/* Some watchdogs have a fixed timeout (no WDIOF_SETTIMEOUT);
         * that is not fatal -- the device is still armed and feedable. */
		if (errno != EOPNOTSUPP && errno != ENOTTY) {
			int e = errno;
			close(fd);
			free(d);
			return _errno_to_alp(e);
		}
	}

	struct watchdog_info info;
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, WDIOC_GETSUPPORT, &info) == 0) {
		d->magic_close = (info.options & WDIOF_MAGICCLOSE) != 0u;
	}

	st->dev         = NULL;
	st->wdt_id      = wdt_id;
	st->channel_id  = 0;
	st->cfg         = *cfg;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/** @brief Kick the watchdog via WDIOC_KEEPALIVE. */
static alp_status_t y_feed(alp_wdt_backend_state_t *st)
{
	y_wdt_data_t *d = (y_wdt_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	int dummy = 0;
	if (ioctl(d->fd, WDIOC_KEEPALIVE, &dummy) < 0) return _errno_to_alp(errno);
	return ALP_OK;
}

/**
 * @brief Disable the running watchdog via WDIOC_SETOPTIONS.
 *
 * Many Linux watchdog drivers are built with NOWAYOUT and reject the
 * disable; WDIOS_DISABLECARD then returns ENOTTY/EOPNOTSUPP, which we
 * surface as ALP_ERR_NOSUPPORT per the alp_wdt_disable contract
 * (one-shot hardware is informational, not a hard error).
 */
static alp_status_t y_disable(alp_wdt_backend_state_t *st)
{
	y_wdt_data_t *d = (y_wdt_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	int flags = WDIOS_DISABLECARD;
	if (ioctl(d->fd, WDIOC_SETOPTIONS, &flags) < 0) {
		if (errno == EOPNOTSUPP) return ALP_ERR_NOSUPPORT;
		return _errno_to_alp(errno);
	}
	return ALP_OK;
}

/**
 * @brief Best-effort disarm, then close the chardev and free state.
 *
 * Honours the alp_wdt_close contract ("best-effort disable, then
 * release").  Tries WDIOS_DISABLECARD first; if the driver advertised
 * magic-close, writes the 'V' character before close() so a
 * non-NOWAYOUT kernel stops the timer instead of letting it keep
 * counting after the fd is gone.
 */
static void y_close(alp_wdt_backend_state_t *st)
{
	y_wdt_data_t *d = (y_wdt_data_t *)st->be_data;
	if (d == NULL) return;
	if (d->fd >= 0) {
		int flags = WDIOS_DISABLECARD;
		(void)ioctl(d->fd, WDIOC_SETOPTIONS, &flags);
		if (d->magic_close) {
			(void)!write(d->fd, "V", 1);
		}
		close(d->fd);
	}
	free(d);
	st->be_data = NULL;
}

static const alp_wdt_ops_t _ops = {
	.open    = y_open,
	.feed    = y_feed,
	.disable = y_disable,
	.close   = y_close,
};

ALP_BACKEND_REGISTER(wdt, yocto_drv,
                     {
                         .silicon_ref = "*",
                         .vendor      = "linux",
                         .base_caps   = 0u,
                         .priority    = 100,
                         .ops         = &_ops,
                         .probe       = NULL,
                     });

#endif /* __linux__ */
