/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto power_* driver-class backend.  Binds the alp_power
 * dispatcher's ops vtable to the kernel's generic sleep-state sysfs ABI
 * (/sys/power/state, Documentation/ABI/testing/sysfs-power) plus the
 * per-RTC wakealarm attribute (/sys/class/rtc/rtc0/wakealarm) for timed
 * wakes.  Pure file I/O -- no vendor HAL, no ioctls.
 *
 * @par Tracking: github.com/alplabai/alp-sdk/issues/613 (this file
 *      closes the Yocto/Linux power backend gap the zephyr_stub.c
 *      wildcard documented -- request_sleep no longer returns
 *      ALP_ERR_NOSUPPORT on a Linux build).
 *
 * ADR 0017: Tier-1 (consumes the stable upstream kernel PM + RTC sysfs
 * ABI as-is -- no vendor HAL, nothing to fork).  Same tier as the
 * sibling RTC/WDT/PWM Yocto backends in this directory tree, which
 * consume /dev/rtcN, /dev/watchdogN and /sys/class/pwm the same way.
 *
 * Registered at priority 100 with vendor "linux"; the wildcard stub
 * (src/backends/power/zephyr_stub.c, priority 0) still wins on
 * non-Linux native_sim builds where this TU compiles to an empty
 * object.
 *
 * @par ALP_POWER_MODE_* -> /sys/power/state mapping
 *
 *      | alp_power_mode_t          | /sys/power/state token |
 *      |---------------------------|------------------------|
 *      | ALP_POWER_MODE_SLEEP      | "freeze"  (S2idle)      |
 *      | ALP_POWER_MODE_DEEP_SLEEP | "standby" (power-on suspend) |
 *      | ALP_POWER_MODE_STANDBY    | "mem"     (suspend-to-RAM)   |
 *
 *      Monotonic depth match against the kernel's own monotonic sleep
 *      ladder (freeze < standby < mem), mirroring the "deeper = lower
 *      power + longer wake" contract <alp/power.h> documents.  Note:
 *      <alp/power.h> describes ALP_POWER_MODE_STANDBY as "RAM NOT
 *      retained", which is closer to the kernel's "disk" (hibernate)
 *      state than "mem" -- but hibernate needs a resume=/swap image
 *      the SDK cannot provision generically, so this backend maps
 *      STANDBY onto the deepest state the generic sysfs ABI always
 *      exposes.  A platform that wants "disk" registers its own
 *      backend at a higher priority.
 *
 * @par Wake handling
 *      wake_after_ms > 0 programs an absolute epoch through
 *      /sys/class/rtc/rtc0/wakealarm: "0" is written first to clear any
 *      stale alarm (the kernel wakealarm attribute rejects programming
 *      a lower absolute value than one already armed), then the target
 *      epoch (now + wake_after_ms, rounded up to whole seconds) is
 *      written.  The /sys/power/state write itself blocks the calling
 *      thread for the duration of the sleep -- that is how the ABI
 *      works, the write() syscall does not return until resume -- so
 *      slept_ms is measured directly around that write.
 *
 * @par Status
 *      REAL implementation.  BENCH-UNVERIFIED (no target in this CI
 *      environment carries a real /sys/power/state +
 *      /sys/class/rtc/rtc0) -- same status as the RTC/WDT/PWM Yocto
 *      slices it sits next to.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "power_ops.h"
#include "common/alp_errno.h"

/* sysfs paths this backend writes.  Explicit constants -- never
 * resolved via $PATH or any other runtime lookup -- overridable at
 * compile time (-DALP_YOCTO_POWER_STATE_PATH=... /
 * -DALP_YOCTO_POWER_WAKEALARM_PATH=...) so a host test can point them
 * at a temp file instead of the real kernel nodes.  Same override
 * pattern as ALP_YOCTO_PWM_CHIP in the sibling PWM backend. */
#ifndef ALP_YOCTO_POWER_STATE_PATH
#define ALP_YOCTO_POWER_STATE_PATH "/sys/power/state"
#endif
#ifndef ALP_YOCTO_POWER_WAKEALARM_PATH
#define ALP_YOCTO_POWER_WAKEALARM_PATH "/sys/class/rtc/rtc0/wakealarm"
#endif

/* Test-only sysfs-write interception (default NULL: real file I/O via
 * open()+write()+close()).  Both attribute writes this backend
 * performs -- /sys/power/state and the wakealarm -- funnel through
 * this single chokepoint, so a test can drive y_request_sleep()
 * deterministically and assert the exact string written, without a
 * real kernel PM/RTC node.  Not part of any public header; only
 * tests/yocto/peripheral_power.c (which #includes this .c file
 * directly) sets it. */
static alp_status_t (*g_power_test_sysfs_write_hook)(const char *path, const char *val) = NULL;

/**
 * @brief Write @p val (a NUL-terminated string) to a sysfs attribute.
 *
 * Opens @p path write-only, writes the whole string, and closes.  A
 * short write or an open failure maps errno -> alp via the shared
 * @ref alp_status_from_posix_errno baseline (#630) -- failures are
 * returned, never swallowed.
 */
static alp_status_t _sysfs_write(const char *path, const char *val)
{
	if (g_power_test_sysfs_write_hook != NULL) return g_power_test_sysfs_write_hook(path, val);

	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return alp_status_from_posix_errno(errno);

	size_t  len = strlen(val);
	ssize_t n   = write(fd, val, len);
	int     e   = errno;
	close(fd);

	if (n < 0) return alp_status_from_posix_errno(e);
	if ((size_t)n != len) return ALP_ERR_IO;
	return ALP_OK;
}

/**
 * @brief Map a portable @ref alp_power_mode_t to the kernel's
 *        /sys/power/state token.  See the file-header mapping table.
 */
static const char *_state_token(alp_power_mode_t mode)
{
	switch (mode) {
	case ALP_POWER_MODE_SLEEP:
		return "freeze";
	case ALP_POWER_MODE_DEEP_SLEEP:
		return "standby";
	case ALP_POWER_MODE_STANDBY:
		return "mem";
	default:
		/* ALP_POWER_MODE_RUN is rejected by the dispatcher before this
	     * op is reached (request_sleep(RUN) is documented invalid);
	     * fall back to the lightest sleep state rather than writing an
	     * undefined token. */
		return "freeze";
	}
}

/* Test-only "now" hook so a test can pin the epoch instead of racing
 * the real wall clock; default NULL uses time(NULL). */
static time_t (*g_power_test_time_hook)(void) = NULL;

static time_t _now(void)
{
	if (g_power_test_time_hook != NULL) return g_power_test_time_hook();
	return time(NULL);
}

/**
 * @brief Program the RTC wakealarm for wake_after_ms from now.
 *
 * Writes "0" first to clear any stale alarm -- the kernel wakealarm
 * attribute rejects programming a new absolute value lower than one
 * already armed, so a leftover alarm from a previous cycle must be
 * cleared unconditionally before the new value is written.  The
 * target is rounded UP to whole seconds (wakealarm has 1 s
 * resolution) so a short wake_after_ms never rounds down to "already
 * passed".
 */
static alp_status_t _program_wakealarm(uint32_t wake_after_ms)
{
	alp_status_t rc = _sysfs_write(ALP_YOCTO_POWER_WAKEALARM_PATH, "0");
	if (rc != ALP_OK) return rc;

	time_t target = _now() + (time_t)((wake_after_ms + 999u) / 1000u);
	char   buf[32];
	int    n = snprintf(buf, sizeof(buf), "%lld", (long long)target);
	if (n < 0 || (size_t)n >= sizeof(buf)) return ALP_ERR_INVAL;

	return _sysfs_write(ALP_YOCTO_POWER_WAKEALARM_PATH, buf);
}

static alp_status_t y_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)state;
	/* The generic sysfs sleep ABI has no queryable capability surface
     * beyond presence, so caps stay 0 -- same as the RTC/WDT backends. */
	if (caps_out != NULL) caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t y_configure_wake_source(alp_power_backend_state_t *state, uint32_t wake_bitmap)
{
	/* The dispatcher already mirrors the bitmap into state->wake_bitmap
     * for its own INVAL guard.  The kernel sysfs sleep ABI has no
     * per-source enable knob beyond the wakealarm this backend
     * programs at request_sleep() time when wake_after_ms > 0, so
     * there is nothing further to configure here -- matches the
     * zephyr_stub / zephyr_pm_policy backends' treatment of this op. */
	(void)state;
	(void)wake_bitmap;
	return ALP_OK;
}

static alp_status_t y_request_sleep(alp_power_backend_state_t *state,
                                    alp_power_mode_t           mode,
                                    uint32_t                   wake_after_ms,
                                    alp_power_wake_info_t     *info)
{
	(void)state;

	if (wake_after_ms > 0u) {
		alp_status_t rc = _program_wakealarm(wake_after_ms);
		if (rc != ALP_OK) return rc;
	}

	/* The /sys/power/state write blocks until resume -- time the
     * write itself for slept_ms rather than estimating. */
	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);
	alp_status_t rc = _sysfs_write(ALP_YOCTO_POWER_STATE_PATH, _state_token(mode));
	clock_gettime(CLOCK_MONOTONIC, &after);

	if (info != NULL) {
		info->realised_mode = mode;
		info->wake_source =
		    (rc == ALP_OK && wake_after_ms > 0u) ? (uint32_t)ALP_POWER_WAKE_RTC : 0u;
		int64_t ms     = (int64_t)(after.tv_sec - before.tv_sec) * 1000 +
		                 (int64_t)(after.tv_nsec - before.tv_nsec) / 1000000;
		info->slept_ms = (uint32_t)((ms > 0) ? ms : 0);
	}

	/* Explicit failure surface: a write() failure (open/short-write/
     * errno) returns the mapped ALP_ERR_* here -- never swallowed into
     * a silent ALP_OK. */
	return rc;
}

static void y_close(alp_power_backend_state_t *state)
{
	(void)state;
}

static const alp_power_ops_t _ops = {
	.open                  = y_open,
	.configure_wake_source = y_configure_wake_source,
	.request_sleep         = y_request_sleep,
	.close                 = y_close,
};

ALP_BACKEND_REGISTER(power,
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
