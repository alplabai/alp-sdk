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
#include "common/alp_errno.h"

/* Per-handle backend data: the open watchdog chardev fd plus whether
 * the driver advertised magic-close support (so close() can disarm
 * via the 'V' write when the platform honours it). */
typedef struct {
	int  fd;
	bool magic_close;
} y_wdt_data_t;

/* Test-only interception of the three syscalls y_open() drives before
 * it has a fully-populated backend handle (open() / malloc() / the
 * WDIOC_SETTIMEOUT ioctl's programmed value), plus observation hooks
 * for the two syscalls _disarm_and_close() uses to actually disarm.
 * Defaults are NULL/real -- production behaviour is unchanged.  Opening
 * a REAL /dev/watchdogN here would arm a genuine hardware watchdog on
 * whatever host runs the test (there usually isn't one, but on a board
 * that has one this must never be exercised for real), so
 * tests/yocto/peripheral_wdt.c (which #includes this .c file directly)
 * drives these seams instead of a real device node (#760).
 *
 * The disarm seams INTERCEPT their syscall rather than observing it
 * from alongside, for the same reason _wdt_open()/_wdt_malloc() do: a
 * hook that merely sits next to the call still fires when the call is
 * deleted, so it can only ever prove "_disarm_and_close() ran", never
 * "the watchdog was disarmed" -- which is the whole of #760.  Routing
 * the syscall THROUGH the seam makes the two inseparable: delete the
 * disarm and the hook goes with it, turning the test red. */
static int (*g_wdt_test_open_hook)(const char *path)              = NULL;
static void *(*g_wdt_test_malloc_hook)(size_t size)               = NULL;
static void (*g_wdt_test_settimeout_observed_hook)(int timeout_s) = NULL;
static int (*g_wdt_test_setoptions_hook)(int fd, int flags)       = NULL;
static ssize_t (*g_wdt_test_magic_write_hook)(int fd)             = NULL;

static int _wdt_open(const char *path)
{
	if (g_wdt_test_open_hook != NULL) return g_wdt_test_open_hook(path);
	return open(path, O_WRONLY | O_CLOEXEC);
}

static void *_wdt_malloc(size_t size)
{
	if (g_wdt_test_malloc_hook != NULL) return g_wdt_test_malloc_hook(size);
	return malloc(size);
}

/* Scoped to the disarm path on purpose: y_open()'s own SETTIMEOUT /
 * GETSUPPORT ioctls must keep reaching the real kernel (or the pipe
 * fd that stands in for it), so this seam does not wrap ioctl() in
 * general -- only the WDIOS_DISABLECARD that #760 is about. */
static int _wdt_disarm_setoptions(int fd, int flags)
{
	if (g_wdt_test_setoptions_hook != NULL) return g_wdt_test_setoptions_hook(fd, flags);
	return ioctl(fd, WDIOC_SETOPTIONS, &flags);
}

static ssize_t _wdt_magic_write(int fd)
{
	if (g_wdt_test_magic_write_hook != NULL) return g_wdt_test_magic_write_hook(fd);
	return write(fd, "V", 1);
}

/**
 * @brief Best-effort disarm (WDIOS_DISABLECARD + magic 'V' close), then
 *        close the chardev.
 *
 * Centralises the disarm-before-release sequence (#760) so EVERY
 * post-open failure in y_open(), not just the ordinary y_close() path,
 * attempts to leave the kernel watchdog disabled before the fd goes
 * away -- opening /dev/watchdogN arms most Linux watchdogs immediately,
 * so a bail-out that only closes the descriptor abandons an armed timer
 * the application now has no handle to feed or disable.
 * @p attempt_magic_close lets early failures (before WDIOC_GETSUPPORT has
 * run) skip the magic-close write when support is still unknown --
 * WDIOS_DISABLECARD is still attempted unconditionally.  Errors are
 * swallowed; this function has no return (matches y_close's contract).
 */
static void _disarm_and_close(int fd, bool attempt_magic_close)
{
	if (fd < 0) return;
	(void)_wdt_disarm_setoptions(fd, WDIOS_DISABLECARD);
	if (attempt_magic_close) {
		(void)!_wdt_magic_write(fd);
	}
	close(fd);
}

/**
 * @brief Open /dev/watchdog<cfg->wdt_id>, program the timeout, read caps.
 *
 * Opening the node arms the watchdog immediately (Linux semantics).
 * The alp config carries wdt_id + timeout_ms; WDIOC_SETTIMEOUT works
 * in whole seconds, so we round up to at least 1 s using an
 * overflow-free quotient/remainder ceiling division (#760: the naive
 * `(timeout_ms + 999u) / 1000u` wraps uint32_t for timeout_ms close to
 * UINT32_MAX and silently programs the 1 s floor instead of ~49.7
 * days).  WDIOC_GETSUPPORT tells us whether magic-close is available
 * for the close()-disarm path.  Errors route through the shared @ref
 * alp_status_from_posix_errno baseline (#630) -- a missing
 * /dev/watchdogN (ENOENT/ENODEV) now correctly surfaces as
 * ALP_ERR_NOT_READY instead of falling through to ALP_ERR_IO, which
 * this backend's now-removed local mapping switch didn't cover.  Every
 * failure from here on attempts a best-effort disarm via
 * @ref _disarm_and_close before releasing the fd (#760) so a failed
 * open never abandons an armed, unfeedable watchdog.
 */
static alp_status_t
y_open(const alp_wdt_config_t *cfg, alp_wdt_backend_state_t *st, alp_capabilities_t *caps_out)
{
	char path[32];
	int  n = snprintf(path, sizeof(path), "/dev/watchdog%u", (unsigned)cfg->wdt_id);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	int fd = _wdt_open(path);
	if (fd < 0) return alp_status_from_posix_errno(errno);

	y_wdt_data_t *d = (y_wdt_data_t *)_wdt_malloc(sizeof(*d));
	if (d == NULL) {
		/* magic-close support is unknown this early (GETSUPPORT hasn't
         * run yet) -- DISABLECARD is still attempted. */
		_disarm_and_close(fd, false);
		return ALP_ERR_NOMEM;
	}
	d->fd          = fd;
	d->magic_close = false;

	/* Ceiling-divide timeout_ms to whole seconds (min 1 s) without the
     * uint32_t overflow the old `(timeout_ms + 999u) / 1000u` formula
     * had at the top of the range; the dispatcher already rejected a
     * zero timeout_ms before reaching the backend. */
	uint32_t timeout_s_u32 = cfg->timeout_ms / 1000u;
	uint32_t rem_ms        = cfg->timeout_ms % 1000u;
	if (rem_ms != 0u) timeout_s_u32 += 1u;
	if (timeout_s_u32 < 1u) timeout_s_u32 = 1u;
	int timeout_s = (int)timeout_s_u32;
	if (g_wdt_test_settimeout_observed_hook != NULL) {
		g_wdt_test_settimeout_observed_hook(timeout_s);
	}
	if (ioctl(fd, WDIOC_SETTIMEOUT, &timeout_s) < 0) {
		/* Some watchdogs have a fixed timeout (no WDIOF_SETTIMEOUT);
         * that is not fatal -- the device is still armed and feedable. */
		if (errno != EOPNOTSUPP && errno != ENOTTY) {
			int e = errno; /* save before _disarm_and_close's own ioctl/write/close touch it */
			_disarm_and_close(fd, false); /* magic-close support still unknown here */
			free(d);
			return alp_status_from_posix_errno(e);
		}
	}

	struct watchdog_info info;
	memset(&info, 0, sizeof(info));
	if (ioctl(fd, WDIOC_GETSUPPORT, &info) == 0) {
		d->magic_close = (info.options & WDIOF_MAGICCLOSE) != 0u;
	}

	st->dev         = NULL;
	st->wdt_id      = cfg->wdt_id;
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
	if (ioctl(d->fd, WDIOC_KEEPALIVE, &dummy) < 0) return alp_status_from_posix_errno(errno);
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
		return alp_status_from_posix_errno(errno);
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
	_disarm_and_close(d->fd, d->magic_close);
	free(d);
	st->be_data = NULL;
}

static const alp_wdt_ops_t _ops = {
	.open    = y_open,
	.feed    = y_feed,
	.disable = y_disable,
	.close   = y_close,
};

ALP_BACKEND_REGISTER(wdt,
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
