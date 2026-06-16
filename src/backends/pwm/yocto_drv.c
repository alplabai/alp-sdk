/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto pwm_* driver-class backend.  Binds the alp_pwm
 * dispatcher's ops vtable to the kernel generic PWM sysfs interface
 * under /sys/class/pwm/pwmchip<chip>/ (the stable ABI documented in
 * Documentation/ABI/testing/sysfs-class-pwm).  Pure file I/O:
 *
 *   export                 -- write the channel index to request it
 *   pwm<ch>/period         -- total cycle length, nanoseconds
 *   pwm<ch>/duty_cycle     -- active length, nanoseconds (<= period)
 *   pwm<ch>/polarity       -- "normal" | "inversed"
 *   pwm<ch>/enable         -- "1" to run, "0" to stop
 *   unexport               -- write the channel index to release it
 *
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds
 * where this TU compiles to an empty object.
 *
 * Selected on any silicon (silicon_ref "*") because the sysfs PWM ABI
 * is SoC-agnostic; the device-tree / kernel decides which physical
 * timer-compare block backs each pwmchip.
 *
 * @par Channel-id mapping
 *      alp_pwm channel_id 0..7 (the studio-resolved E1M PWM index) maps
 *      directly to the sysfs channel index on a single configurable
 *      pwmchip.  The chip number defaults to 0 and can be overridden at
 *      build time via ALP_YOCTO_PWM_CHIP so an integration can pin the
 *      class to a non-zero pwmchip without a code change.
 *
 * @par Status
 *      REAL implementation.  Yocto-link + on-target run BENCH-UNVERIFIED
 *      (no sysroot / no real /sys/class/pwm device nodes in this CI
 *      environment) -- exactly like the RTC/WDT slice.
 *
 * @par Unsupported ops
 *      configure(), single_pulse(), capture_open(), capture_read() and
 *      capture_close() return ALP_ERR_NOSUPPORT: the generic PWM sysfs
 *      ABI exposes only period / duty_cycle / polarity / enable.  It has
 *      NO standard sysfs entry for dead-time, center-aligned counters,
 *      a one-shot/single-pulse primitive, or input capture, so faking
 *      those would mean inventing non-standard paths.  See the per-op
 *      comments below.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>
#include <alp/pwm.h>

#include "pwm_ops.h"

/* sysfs pwmchip the class binds to.  Override at build time
 * (-DALP_YOCTO_PWM_CHIP=N) to pin a non-zero chip. */
#ifndef ALP_YOCTO_PWM_CHIP
#define ALP_YOCTO_PWM_CHIP 0
#endif

/* Recover the enclosing portable handle from its embedded state slot.
 * The dispatcher reads h->period_ns to bounds-check pulse_ns before
 * forwarding here, so open() must populate it.  <zephyr/sys/util.h>'s
 * CONTAINER_OF is unavailable on Linux, so define the offset locally. */
#define ALP_PWM_HANDLE_OF(st_ptr)                                                                  \
	((struct alp_pwm *)((char *)(st_ptr)-offsetof(struct alp_pwm, state)))

/* Per-handle backend data: the channel's sysfs directory and channel
 * index, boxed onto the heap so the void* be_data slot owns it. */
typedef struct {
	char dir[64]; /* "/sys/class/pwm/pwmchip<chip>/pwm<ch>" */
	int  chip;    /* pwmchip number */
	int  channel; /* sysfs channel index */
} y_pwm_data_t;

/** @brief Map a (positive) errno value to the closest alp_status_t. */
static alp_status_t _errno_to_alp(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case EINVAL:
		return ALP_ERR_INVAL;
	case EBUSY:
	case EAGAIN:
		return ALP_ERR_BUSY;
	case ENODEV:
	case ENOENT:
		return ALP_ERR_NOT_READY;
	case ENOTTY:
	case ENOSYS:
	case ENOTSUP:
#if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
	case EOPNOTSUPP:
#endif
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

/**
 * @brief Write @p val (a NUL-terminated string) to a sysfs attribute.
 *
 * Opens @p path write-only, writes the whole string, and closes.  A
 * short write or an open failure maps errno -> alp via @ref
 * _errno_to_alp.  Used for export / unexport / enable / polarity and
 * the decimal-formatted period / duty_cycle writes.
 */
static alp_status_t _sysfs_write(const char *path, const char *val)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return _errno_to_alp(errno);

	size_t  len = strlen(val);
	ssize_t n   = write(fd, val, len);
	int     e   = errno;
	close(fd);

	if (n < 0) return _errno_to_alp(e);
	if ((size_t)n != len) return ALP_ERR_IO;
	return ALP_OK;
}

/**
 * @brief Write a decimal nanosecond value to a per-channel attribute.
 *
 * Formats @p attr ("period" or "duty_cycle") under the handle's cached
 * channel directory and writes @p ns as base-10 ASCII, the form the
 * sysfs PWM ABI expects.
 */
static alp_status_t _write_ns(const y_pwm_data_t *d, const char *attr, uint32_t ns)
{
	char path[96];
	char buf[16];

	int n = snprintf(path, sizeof(path), "%s/%s", d->dir, attr);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	n = snprintf(buf, sizeof(buf), "%u", (unsigned)ns);
	if (n < 0 || (size_t)n >= sizeof(buf)) return ALP_ERR_INVAL;

	return _sysfs_write(path, buf);
}

/**
 * @brief Best-effort release of a channel via pwmchip<chip>/unexport.
 *
 * Writes @p channel to the chip's unexport attribute so the channel can
 * be re-acquired cleanly.  Used both on the y_open() error paths (after
 * a successful export) and from y_close(); all errors are swallowed.
 */
static void _unexport_chip(int chip, int channel)
{
	char unexp[64];
	char idx[16];
	int  n = snprintf(unexp, sizeof(unexp), "/sys/class/pwm/pwmchip%d/unexport", chip);
	int  m = snprintf(idx, sizeof(idx), "%d", channel);
	if (n > 0 && (size_t)n < sizeof(unexp) && m > 0 && (size_t)m < sizeof(idx)) {
		(void)_sysfs_write(unexp, idx);
	}
}

/**
 * @brief Export the channel, set period + polarity, prime 0 % duty, enable.
 *
 * Writes the channel index to pwmchip<chip>/export (EBUSY is tolerated:
 * an already-exported channel is reusable), then programs period,
 * polarity and a 0 ns duty_cycle before enabling.  The sysfs ABI has no
 * queryable capability surface, so caps stay 0.
 *
 * Note the ABI ordering constraint: duty_cycle must be <= period, so
 * period is written first and duty_cycle is left at 0 here.
 */
static alp_status_t
y_open(const alp_pwm_config_t *cfg, alp_pwm_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg->channel_id >= 8u) return ALP_ERR_OUT_OF_RANGE;

	y_pwm_data_t *d = (y_pwm_data_t *)malloc(sizeof(*d));
	if (d == NULL) return ALP_ERR_NOMEM;
	d->chip    = ALP_YOCTO_PWM_CHIP;
	d->channel = (int)cfg->channel_id;

	char chipdir[48];
	int  n = snprintf(chipdir, sizeof(chipdir), "/sys/class/pwm/pwmchip%d", d->chip);
	if (n < 0 || (size_t)n >= sizeof(chipdir)) {
		free(d);
		return ALP_ERR_INVAL;
	}

	n = snprintf(d->dir, sizeof(d->dir), "%s/pwm%d", chipdir, d->channel);
	if (n < 0 || (size_t)n >= sizeof(d->dir)) {
		free(d);
		return ALP_ERR_INVAL;
	}

	/* Request the channel.  EBUSY == already exported -> reuse it. */
	char exp_path[64];
	char idx[16];
	n = snprintf(exp_path, sizeof(exp_path), "%s/export", chipdir);
	if (n < 0 || (size_t)n >= sizeof(exp_path)) {
		free(d);
		return ALP_ERR_INVAL;
	}
	n = snprintf(idx, sizeof(idx), "%d", d->channel);
	if (n < 0 || (size_t)n >= sizeof(idx)) {
		free(d);
		return ALP_ERR_INVAL;
	}

	alp_status_t rc = _sysfs_write(exp_path, idx);
	if (rc != ALP_OK && rc != ALP_ERR_BUSY) {
		free(d);
		return rc;
	}

	struct alp_pwm *h = ALP_PWM_HANDLE_OF(st);
	h->channel        = cfg->channel_id;
	h->period_ns      = (cfg->period_ns != 0u) ? cfg->period_ns : 1000000u; /* 1 kHz */
	h->flags          = (uint32_t)cfg->polarity;

	/* polarity is write-rejected by some drivers while enabled, so set
     * it before enabling.  EINVAL/ENOTSUP here is non-fatal: not every
     * driver allows polarity inversion. */
	const char *pol = (cfg->polarity == ALP_PWM_POLARITY_INVERTED) ? "inversed" : "normal";
	char        pol_path[96];
	n = snprintf(pol_path, sizeof(pol_path), "%s/polarity", d->dir);
	if (n > 0 && (size_t)n < sizeof(pol_path)) {
		(void)_sysfs_write(pol_path, pol); /* best-effort */
	}

	/* period before duty_cycle (ABI: duty_cycle <= period).  On any
     * post-export failure, unexport the channel (best-effort) before
     * returning so a later retry does not inherit a half-configured
     * channel (period set but never enabled, or stale duty); see
     * _unexport_chip() / y_close()'s unexport. */
	rc = _write_ns(d, "period", h->period_ns);
	if (rc != ALP_OK) {
		_unexport_chip(d->chip, d->channel);
		free(d);
		return rc;
	}
	rc = _write_ns(d, "duty_cycle", 0u);
	if (rc != ALP_OK) {
		_unexport_chip(d->chip, d->channel);
		free(d);
		return rc;
	}

	/* Enable so subsequent set_duty takes effect immediately; output
     * stays low at 0 % duty until the caller arms it. */
	char en_path[96];
	n = snprintf(en_path, sizeof(en_path), "%s/enable", d->dir);
	if (n > 0 && (size_t)n < sizeof(en_path)) {
		rc = _sysfs_write(en_path, "1");
		if (rc != ALP_OK) {
			_unexport_chip(d->chip, d->channel);
			free(d);
			return rc;
		}
	}

	st->dev         = NULL;
	st->channel_id  = cfg->channel_id;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/**
 * @brief Set the active-level pulse width by writing duty_cycle (ns).
 *
 * The dispatcher already guarantees pulse_ns <= h->period_ns, which
 * satisfies the sysfs duty_cycle <= period ABI constraint.
 */
static alp_status_t y_set_duty(alp_pwm_backend_state_t *st, uint32_t pulse_ns)
{
	y_pwm_data_t *d = (y_pwm_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;
	return _write_ns(d, "duty_cycle", pulse_ns);
}

/**
 * @brief Update the period; reset duty to 0 % per the documented contract.
 *
 * duty_cycle is written to 0 first so the new period can never be
 * smaller than a stale duty (the kernel rejects period < duty_cycle).
 * The dispatcher caches period_ns on an ALP_OK return.
 */
static alp_status_t y_set_period(alp_pwm_backend_state_t *st, uint32_t period_ns)
{
	y_pwm_data_t *d = (y_pwm_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	alp_status_t rc = _write_ns(d, "duty_cycle", 0u);
	if (rc != ALP_OK) return rc;
	return _write_ns(d, "period", period_ns);
}

/**
 * @brief Sticky tuning -- unsupported on the generic PWM sysfs ABI.
 *
 * The sysfs PWM interface exposes only period / duty_cycle / polarity /
 * enable.  It has NO standard attribute for dead-time or center-aligned
 * counter modes, so the call is refused rather than silently no-op'd.
 */
static alp_status_t y_configure(alp_pwm_backend_state_t *st,
                                alp_pwm_align_t          align_mode,
                                uint32_t                 dead_time_ns,
                                uint8_t                  break_cfg)
{
	(void)st;
	(void)align_mode;
	(void)dead_time_ns;
	(void)break_cfg;
	return ALP_ERR_NOSUPPORT;
}

/**
 * @brief One-shot pulse -- unsupported on the generic PWM sysfs ABI.
 *
 * The sysfs PWM interface has no single-shot / one-pulse primitive
 * (no standard "oneshot" attribute), so the call is refused rather
 * than faking it with a sleep + disable race.
 */
static alp_status_t y_single_pulse(alp_pwm_backend_state_t *st, uint32_t pulse_ns)
{
	(void)st;
	(void)pulse_ns;
	return ALP_ERR_NOSUPPORT;
}

/**
 * @brief Input capture open -- unsupported on the generic PWM sysfs ABI.
 *
 * The sysfs PWM interface is output-only; it exposes no edge-capture
 * surface.  Input-capture on Linux lives in unrelated subsystems
 * (IIO triggered buffers, the Counter subsystem), not /sys/class/pwm,
 * so this entry is refused.
 */
static alp_status_t y_capture_open(const alp_pwm_capture_config_t *cfg,
                                   alp_pwm_backend_state_t        *st,
                                   alp_capabilities_t             *caps_out)
{
	(void)cfg;
	(void)st;
	(void)caps_out;
	return ALP_ERR_NOSUPPORT;
}

/** @brief Input capture read -- unsupported (see @ref y_capture_open). */
static alp_status_t
y_capture_read(alp_pwm_backend_state_t *st, uint32_t *period_ns_out, uint32_t *pulse_ns_out)
{
	(void)st;
	if (period_ns_out != NULL) *period_ns_out = 0u;
	if (pulse_ns_out != NULL) *pulse_ns_out = 0u;
	return ALP_ERR_NOSUPPORT;
}

/** @brief Input capture close -- no-op (capture is unsupported). */
static void y_capture_close(alp_pwm_backend_state_t *st)
{
	(void)st;
}

/**
 * @brief Disable the channel, unexport it, and free the per-handle box.
 *
 * Best-effort: writes "0" to enable, then the channel index to
 * pwmchip<chip>/unexport so the channel can be re-acquired cleanly,
 * then frees the heap box.  Errors are swallowed -- close has no return.
 */
static void y_close(alp_pwm_backend_state_t *st)
{
	y_pwm_data_t *d = (y_pwm_data_t *)st->be_data;
	if (d == NULL) return;

	char path[96];
	int  n = snprintf(path, sizeof(path), "%s/enable", d->dir);
	if (n > 0 && (size_t)n < sizeof(path)) {
		(void)_sysfs_write(path, "0");
	}

	_unexport_chip(d->chip, d->channel);

	free(d);
	st->be_data = NULL;
}

static const alp_pwm_ops_t _ops = {
	.open          = y_open,
	.set_duty      = y_set_duty,
	.set_period    = y_set_period,
	.configure     = y_configure,
	.single_pulse  = y_single_pulse,
	.capture_open  = y_capture_open,
	.capture_read  = y_capture_read,
	.capture_close = y_capture_close,
	.close         = y_close,
};

ALP_BACKEND_REGISTER(pwm,
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
