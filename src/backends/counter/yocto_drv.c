/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto counter_* driver-class backend.  Binds the
 * alp_counter dispatcher's ops vtable to the Linux Counter subsystem
 * sysfs ABI under /sys/bus/counter/devices/counter<N>/ (documented in
 * the kernel tree at Documentation/ABI/testing/sysfs-bus-counter).
 *
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds where
 * this TU compiles to an empty object.
 *
 * Selected on any silicon (silicon_ref "*") because the Counter sysfs
 * ABI is SoC-agnostic; the device tree / kernel decides which physical
 * counter hardware backs counter<N>.
 *
 * Status: REAL implementation against the standard Counter sysfs ABI.
 * Yocto-link and on-target run are BENCH-UNVERIFIED — there is no
 * sysroot and no real /sys/bus/counter device node in this tree.
 *
 * The Linux Counter subsystem is the LEAST-standardised of the classes
 * migrated in this slice.  The sysfs ABI exposes a Count value
 * (count<M>/count) and a per-Count enable (count<M>/enable), but it has
 * NO standard surface for:
 *   - a tick-frequency the host can read (no us<->ticks conversion),
 *   - a hardware deadline / alarm callback,
 *   - a device-wide start/stop distinct from per-Count enable.
 * Every op that would require one of those non-existent ABI entries
 * returns ALP_ERR_NOSUPPORT with a comment, rather than inventing a
 * sysfs path.  Only count<M>/enable and count<M>/count are touched.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/counter.h>
#include <alp/peripheral.h>

#include "counter_ops.h"

/* Count index within a Counter device.  The alp contract surfaces a
 * single counter per handle, so we bind to Count 0 (count0/) of the
 * resolved counter<N> device.  A device that numbers its Counts
 * differently is not addressable through the alp_counter contract;
 * that is a known limitation of the single-Count mapping, not faked. */
#define Y_COUNTER_COUNT_INDEX 0

/* Per-handle backend data: the resolved sysfs directory for the device,
 * plus the Count index.  Boxed onto the heap so the void* be_data slot
 * in alp_counter_backend_state_t owns it. */
typedef struct {
	char     dir[64];   /* /sys/bus/counter/devices/counter<N> */
	unsigned count_idx; /* Count index within the device */
} y_counter_data_t;

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
	case ENOENT:
	case ENODEV:
		return ALP_ERR_NOT_READY;
	case ENOTTY:
	case ENOSYS:
	case EOPNOTSUPP:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

/**
 * @brief Write a NUL-terminated string to a Counter sysfs attribute.
 *
 * @param dir   Device directory (.../counter<N>).
 * @param attr  Attribute path relative to @p dir (e.g. "count0/enable").
 * @param val   Value string to write.
 * @return ALP_OK or an errno-mapped status.
 */
static alp_status_t _sysfs_write(const char *dir, const char *attr, const char *val)
{
	char path[128];
	int  n = snprintf(path, sizeof(path), "%s/%s", dir, attr);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	int fd = open(path, O_WRONLY | O_CLOEXEC);
	if (fd < 0) return _errno_to_alp(errno);

	size_t  len = strlen(val);
	ssize_t w   = write(fd, val, len);
	int     e   = errno;
	close(fd);
	if (w < 0) return _errno_to_alp(e);
	if ((size_t)w != len) return ALP_ERR_IO;
	return ALP_OK;
}

/**
 * @brief Read a Counter sysfs attribute as an unsigned 32-bit value.
 *
 * @param[in]  dir   Device directory (.../counter<N>).
 * @param[in]  attr  Attribute path relative to @p dir.
 * @param[out] out   Receives the parsed value.
 * @return ALP_OK or an errno-mapped status; ALP_ERR_IO on a malformed read.
 */
static alp_status_t _sysfs_read_u32(const char *dir, const char *attr, uint32_t *out)
{
	char path[128];
	int  n = snprintf(path, sizeof(path), "%s/%s", dir, attr);
	if (n < 0 || (size_t)n >= sizeof(path)) return ALP_ERR_INVAL;

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) return _errno_to_alp(errno);

	char    buf[32];
	ssize_t r = read(fd, buf, sizeof(buf) - 1);
	int     e = errno;
	close(fd);
	if (r < 0) return _errno_to_alp(e);

	buf[r]            = '\0';
	char         *end = NULL;
	unsigned long v   = strtoul(buf, &end, 0);
	if (end == buf) return ALP_ERR_IO; /* no digits parsed */
	*out = (uint32_t)v;
	return ALP_OK;
}

/**
 * @brief Resolve counter<id> under /sys/bus/counter/devices and stash
 *        the directory in the handle state.
 *
 * The Counter sysfs ABI advertises no queryable host-visible capability
 * surface (no tick frequency, no hardware-alarm flag), so caps stay 0.
 * Presence is checked by stat-ing the device directory's count<M>/count
 * attribute via access().
 */
static alp_status_t y_open(const alp_counter_config_t  *cfg,
                           alp_counter_backend_state_t *st,
                           alp_capabilities_t          *caps_out)
{
	if (cfg == NULL || st == NULL || caps_out == NULL) return ALP_ERR_INVAL;

	y_counter_data_t *d = (y_counter_data_t *)malloc(sizeof(*d));
	if (d == NULL) return ALP_ERR_NOMEM;

	d->count_idx = Y_COUNTER_COUNT_INDEX;
	int n        = snprintf(
	    d->dir, sizeof(d->dir), "/sys/bus/counter/devices/counter%u", (unsigned)cfg->counter_id);
	if (n < 0 || (size_t)n >= sizeof(d->dir)) {
		free(d);
		return ALP_ERR_INVAL;
	}

	/* Confirm the device + Count 0 exist by probing count<M>/count. */
	char probe[128];
	n = snprintf(probe, sizeof(probe), "%s/count%u/count", d->dir, d->count_idx);
	if (n < 0 || (size_t)n >= sizeof(probe)) {
		free(d);
		return ALP_ERR_INVAL;
	}
	if (access(probe, F_OK) != 0) {
		int e = errno;
		free(d);
		return _errno_to_alp(e);
	}

	st->dev         = NULL;
	st->counter_id  = cfg->counter_id;
	st->be_data     = d;
	caps_out->flags = 0u;
	return ALP_OK;
}

/**
 * @brief Start the counter by enabling Count <M>.
 *
 * Maps to count<M>/enable = 1.  The "enable" attribute is part of the
 * standard Counter ABI (it gates whether the Count is active).
 */
static alp_status_t y_start(alp_counter_backend_state_t *st)
{
	y_counter_data_t *d = (y_counter_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	char attr[32];
	int  n = snprintf(attr, sizeof(attr), "count%u/enable", d->count_idx);
	if (n < 0 || (size_t)n >= sizeof(attr)) return ALP_ERR_INVAL;
	return _sysfs_write(d->dir, attr, "1\n");
}

/**
 * @brief Stop the counter by disabling Count <M>.
 *
 * Maps to count<M>/enable = 0.  The current value is preserved by the
 * hardware; the alp contract guarantees the value survives stop.
 */
static alp_status_t y_stop(alp_counter_backend_state_t *st)
{
	y_counter_data_t *d = (y_counter_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	char attr[32];
	int  n = snprintf(attr, sizeof(attr), "count%u/enable", d->count_idx);
	if (n < 0 || (size_t)n >= sizeof(attr)) return ALP_ERR_INVAL;
	return _sysfs_write(d->dir, attr, "0\n");
}

/**
 * @brief Read the current tick count from count<M>/count.
 *
 * The "count" attribute is the canonical readable Count value in the
 * Counter sysfs ABI.  Reported as an unsigned 32-bit tick count, which
 * matches the alp_counter contract.
 */
static alp_status_t y_get_value(alp_counter_backend_state_t *st, uint32_t *ticks_out)
{
	if (ticks_out == NULL) return ALP_ERR_INVAL;
	y_counter_data_t *d = (y_counter_data_t *)st->be_data;
	if (d == NULL) return ALP_ERR_NOT_READY;

	char attr[32];
	int  n = snprintf(attr, sizeof(attr), "count%u/count", d->count_idx);
	if (n < 0 || (size_t)n >= sizeof(attr)) return ALP_ERR_INVAL;
	return _sysfs_read_u32(d->dir, attr, ticks_out);
}

/**
 * @brief Convert microseconds to ticks — UNSUPPORTED on Linux Counter.
 *
 * The Counter sysfs ABI exposes no standard host-readable tick
 * frequency: the meaning of a "tick" depends on the underlying Signal
 * source (an external quadrature edge, a timer clock, etc.) and there
 * is no documented sysfs attribute (e.g. a "frequency" or clock-rate
 * entry on the Count) from which to derive a conversion factor.  We do
 * NOT invent one — return ALP_ERR_NOSUPPORT per the alp_counter
 * contract, which explicitly allows this on backends that don't
 * advertise the tick frequency.
 */
static alp_status_t y_us_to_ticks(alp_counter_backend_state_t *st, uint32_t us, uint32_t *ticks_out)
{
	(void)st;
	(void)us;
	if (ticks_out != NULL) *ticks_out = 0u;
	return ALP_ERR_NOSUPPORT;
}

/**
 * @brief Schedule a one-shot alarm — UNSUPPORTED on Linux Counter.
 *
 * The Counter sysfs ABI has no standard deadline / match-and-fire
 * mechanism that delivers an asynchronous callback to userspace at a
 * given tick value.  (Some drivers expose count<M>/watch events via the
 * /dev/counterN chardev's event-watch ioctls — COUNTER_ADD_WATCH_IOCTL
 * etc. — but that interface is per-driver, event-typed, and not the
 * "fire once at ticks_from_now" semantic the alp contract describes, so
 * binding to it would be inventing behaviour the ABI does not promise.)
 * Return ALP_ERR_NOSUPPORT, which the contract permits on backends with
 * no ISR-context callback path.
 */
static alp_status_t
y_set_alarm(alp_counter_backend_state_t *st, uint32_t ticks_from_now, struct alp_counter *owner)
{
	(void)st;
	(void)ticks_from_now;
	(void)owner;
	return ALP_ERR_NOSUPPORT;
}

/**
 * @brief Cancel a pending alarm — no-op on Linux Counter.
 *
 * No alarm can ever be armed via @ref y_set_alarm on this backend, so
 * there is nothing to cancel.  The alp_counter_cancel_alarm contract
 * documents only ALP_OK / ALP_ERR_INVAL / ALP_ERR_NOT_READY and defines
 * the op as a no-op when no alarm is armed, so a genuine no-op returns
 * ALP_OK (matching the sw_fallback) rather than the undocumented
 * NOSUPPORT.  (y_set_alarm's NOSUPPORT is contract-permitted; cancel's
 * is not.)
 */
static alp_status_t y_cancel_alarm(alp_counter_backend_state_t *st)
{
	(void)st;
	return ALP_OK;
}

/**
 * @brief Disable the Count (best-effort) and free the per-handle box.
 *
 * Close mirrors @ref y_stop's enable=0 write, then frees the boxed
 * device-directory state.  Write errors on the enable attribute are
 * ignored — the handle is being torn down regardless.
 */
static void y_close(alp_counter_backend_state_t *st)
{
	y_counter_data_t *d = (y_counter_data_t *)st->be_data;
	if (d != NULL) {
		char attr[32];
		int  n = snprintf(attr, sizeof(attr), "count%u/enable", d->count_idx);
		if (n > 0 && (size_t)n < sizeof(attr)) {
			(void)_sysfs_write(d->dir, attr, "0\n");
		}
		free(d);
		st->be_data = NULL;
	}
}

static const alp_counter_ops_t _ops = {
	.open         = y_open,
	.start        = y_start,
	.stop         = y_stop,
	.get_value    = y_get_value,
	.us_to_ticks  = y_us_to_ticks,
	.set_alarm    = y_set_alarm,
	.cancel_alarm = y_cancel_alarm,
	.close        = y_close,
};

ALP_BACKEND_REGISTER(counter,
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
