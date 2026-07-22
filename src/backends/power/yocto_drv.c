/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto power_* driver-class backend (issue #613).  Binds
 * the alp_power dispatcher's request_sleep to the kernel's standard
 * system-sleep sysfs ABI:
 *
 *   /sys/power/state              -- write "freeze" | "mem" to enter
 *                                     a sleep state (Documentation/
 *                                     ABI/testing/sysfs-power, PM_SUSPEND
 *                                     writeable states)
 *   /sys/class/rtc/rtc0/wakealarm -- write 0 to clear, then an
 *                                     absolute UNIX epoch (decimal
 *                                     seconds) to arm a timed wake
 *                                     (Documentation/ABI/testing/
 *                                     sysfs-class-rtc)
 *
 * Registered at priority 100 with vendor "linux"; the zephyr_stub
 * backend (priority 0) still wins on non-Linux native_sim builds
 * where this TU compiles to an empty object.  Selected on any silicon
 * (silicon_ref "*") because both sysfs interfaces are kernel-generic,
 * not SoC-specific.
 *
 * Mode mapping (maintainer decision, issue #613 / tracking #22)
 * --------------------------------------------------------------
 *   ALP_POWER_MODE_SLEEP      -> "freeze" (suspend-to-idle; RAM live)
 *   ALP_POWER_MODE_DEEP_SLEEP -> "mem"    (suspend-to-RAM; RAM retained)
 *   ALP_POWER_MODE_STANDBY    -> ALP_ERR_NOSUPPORT, nothing written.
 *       <alp/power.h> documents STANDBY as "RAM NOT retained"; Linux's
 *       "mem" state retains RAM, and the state that doesn't ("disk" /
 *       hibernate) is explicitly out of v1.0 scope (tracking #22).
 *       Mapping STANDBY to "mem" would silently break the documented
 *       contract, so this backend refuses rather than approximate it.
 *
 * RTC selection
 * -------------
 * alp_power_open() takes no device selector, so this backend hardcodes
 * rtc0 -- the same "most SoMs expose exactly one RTC, pass rtc_id=0"
 * precedent <alp/rtc.h> documents.
 *
 * Wakealarm sequencing
 * ---------------------
 * The kernel wakealarm attribute rejects a new absolute time while a
 * previous one is still armed, so every arm is preceded by a clear
 * ("echo 0"); the alarm is cleared again after wake so it never
 * outlives this request.  Only armed when @c wake_after_ms > 0 --
 * mirroring the zephyr_pm_policy backend's "timer is implicit when
 * wake_after_ms > 0" contract from <alp/power.h>.
 *
 * Testability (no root needed)
 * -----------------------------
 * Both sysfs paths are overridable at runtime via the
 * ALP_YOCTO_POWER_SYSFS_ROOT environment variable: when set, the
 * backend writes to "<root>/power/state" and
 * "<root>/class/rtc/rtc0/wakealarm" instead of the real /sys tree, so
 * a unit test can point this at a temp directory (mkdir -p + touch the
 * two fake files) and read back exactly what was written -- see
 * tests/yocto/peripheral_power.c.  Unset (the Yocto-image default)
 * resolves the real kernel paths above.
 *
 * @par Status
 *      REAL implementation.  Yocto-link + on-target suspend/resume run
 *      BENCH-UNVERIFIED (no real /sys/power/state + rtc0 wakealarm in
 *      this CI environment) -- exactly like the RTC/WDT/PWM slices.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/power.h>

#include "power_ops.h"

#include "common/alp_errno.h"

/* Real kernel paths, used when ALP_YOCTO_POWER_SYSFS_ROOT is unset. */
#define ALP_YOCTO_POWER_STATE_PATH_DEFAULT     "/sys/power/state"
#define ALP_YOCTO_POWER_WAKEALARM_PATH_DEFAULT "/sys/class/rtc/rtc0/wakealarm"

/* Test-only sysfs-write interception (default NULL: real file I/O via
 * open()+write()+close()).  Both attribute writes this backend
 * performs -- /sys/power/state and the rtc0 wakealarm -- funnel
 * through this single chokepoint, matching the technique
 * src/backends/pwm/yocto_drv.c already uses (g_pwm_test_sysfs_write_hook).
 * Not part of any public header; only tests/yocto/peripheral_power.c
 * (which #includes this .c file directly) sets it. */
static alp_status_t (*g_power_test_sysfs_write_hook)(const char *path, const char *val) = NULL;

/**
 * @brief Write @p val (a NUL-terminated string) to a sysfs attribute.
 *
 * Opens @p path write-only, writes the whole string, and closes.  A
 * failed open, a failed write, or a short write all map to a non-OK
 * @c alp_status_t -- callers must never treat this as best-effort
 * unless they explicitly ignore the return (see the wakealarm-clear
 * cleanup path below).
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
 * @brief Resolve the /sys/power/state path, honouring the test override.
 */
static void _state_path(char *buf, size_t len)
{
	const char *root = getenv("ALP_YOCTO_POWER_SYSFS_ROOT");
	if (root == NULL || root[0] == '\0') {
		(void)snprintf(buf, len, "%s", ALP_YOCTO_POWER_STATE_PATH_DEFAULT);
	} else {
		(void)snprintf(buf, len, "%s/power/state", root);
	}
}

/**
 * @brief Resolve the rtc0 wakealarm path, honouring the test override.
 *
 * Hardcodes rtc0 -- alp_power_open() has no device selector, matching
 * <alp/rtc.h>'s documented "most SoMs expose exactly one RTC" precedent.
 */
static void _wakealarm_path(char *buf, size_t len)
{
	const char *root = getenv("ALP_YOCTO_POWER_SYSFS_ROOT");
	if (root == NULL || root[0] == '\0') {
		(void)snprintf(buf, len, "%s", ALP_YOCTO_POWER_WAKEALARM_PATH_DEFAULT);
	} else {
		(void)snprintf(buf, len, "%s/class/rtc/rtc0/wakealarm", root);
	}
}

/**
 * @brief Map an ALP_POWER_MODE_* to its /sys/power/state token.
 *
 * @return The state string, or NULL for a mode this backend refuses
 *         (STANDBY -- see the file header comment).
 */
static const char *_state_token(alp_power_mode_t mode)
{
	switch (mode) {
	case ALP_POWER_MODE_SLEEP:
		return "freeze";
	case ALP_POWER_MODE_DEEP_SLEEP:
		return "mem";
	default:
		/* STANDBY (and anything the dispatcher didn't already filter)
		 * has no safe token here. */
		return NULL;
	}
}

/**
 * @brief Clear then arm the rtc0 wakealarm for an absolute wake time.
 *
 * The kernel rejects a new alarm while one is still set, so the clear
 * write always precedes the arm write.  Both writes are error-checked;
 * a failure leaves nothing armed (the clear either already cleared it,
 * or the arm never ran).
 */
static alp_status_t _arm_wakealarm(uint32_t wake_after_ms)
{
	char path[128];
	_wakealarm_path(path, sizeof(path));

	alp_status_t rc = _sysfs_write(path, "0");
	if (rc != ALP_OK) return rc;

	time_t   now   = time(NULL);
	uint64_t delta = ((uint64_t)wake_after_ms + 999u) / 1000u; /* ceil to whole seconds */
	uint64_t epoch = (uint64_t)now + delta;

	char val[32];
	int  n = snprintf(val, sizeof(val), "%llu", (unsigned long long)epoch);
	if (n < 0 || (size_t)n >= sizeof(val)) return ALP_ERR_INVAL;

	return _sysfs_write(path, val);
}

/** @brief Best-effort clear of the rtc0 wakealarm (errors swallowed). */
static void _clear_wakealarm(void)
{
	char path[128];
	_wakealarm_path(path, sizeof(path));
	(void)_sysfs_write(path, "0");
}

static alp_status_t y_open(alp_power_backend_state_t *state, alp_capabilities_t *caps_out)
{
	(void)state;
	/* No queryable capability surface on the sysfs power-state ABI;
	 * no per-handle resource to allocate (every write below opens and
	 * closes its own fd). */
	if (caps_out != NULL) caps_out->flags = 0u;
	return ALP_OK;
}

static alp_status_t y_configure_wake_source(alp_power_backend_state_t *state, uint32_t wake_bitmap)
{
	/* The dispatcher already mirrors the bitmap in state->wake_bitmap;
	 * this backend only ever arms the RTC wakealarm, and only when the
	 * caller passes wake_after_ms > 0 at request_sleep() time (see the
	 * file header comment) -- so there is nothing further to configure
	 * here. */
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

	const char *token = _state_token(mode);
	if (token == NULL) {
		/* STANDBY: refuse before touching any sysfs attribute. */
		if (info != NULL) {
			info->realised_mode = mode;
			info->wake_source   = 0u;
			info->slept_ms      = 0u;
		}
		return ALP_ERR_NOSUPPORT;
	}

	bool arm_rtc = (wake_after_ms > 0u);
	if (arm_rtc) {
		alp_status_t rc = _arm_wakealarm(wake_after_ms);
		if (rc != ALP_OK) return rc;
	}

	struct timespec before, after;
	clock_gettime(CLOCK_MONOTONIC, &before);

	char state_path[128];
	_state_path(state_path, sizeof(state_path));
	/* Blocks here on real hardware until the SoC resumes; the sysfs
	 * write only returns once the suspend/resume round-trip completes. */
	alp_status_t rc = _sysfs_write(state_path, token);

	clock_gettime(CLOCK_MONOTONIC, &after);

	if (arm_rtc) {
		/* Clear regardless of rc: a failed suspend must not leave a
		 * stray alarm armed for some future unrelated wake. */
		_clear_wakealarm();
	}

	if (rc != ALP_OK) return rc;

	if (info != NULL) {
		info->realised_mode = mode;
		info->wake_source   = arm_rtc ? (uint32_t)ALP_POWER_WAKE_RTC : 0u;
		int64_t ms          = (int64_t)(after.tv_sec - before.tv_sec) * 1000 +
		                      (int64_t)(after.tv_nsec - before.tv_nsec) / 1000000;
		info->slept_ms      = (ms > 0) ? (uint32_t)ms : 0u;
	}
	return ALP_OK;
}

static void y_close(alp_power_backend_state_t *state)
{
	(void)state;
	/* Nothing to release: every op above is a self-contained
	 * open+write+close against a sysfs path. */
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
