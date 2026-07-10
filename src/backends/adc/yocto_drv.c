/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Real Linux/Yocto adc_* driver-class backend.  Binds the alp_adc
 * dispatcher's ops vtable to the kernel's Industrial I/O (IIO) sysfs
 * ABI under /sys/bus/iio/devices/iio:device<id>/.  Pure file I/O: the
 * raw conversion code comes from in_voltage<ch>_raw and the
 * volt-per-count scale from in_voltage<ch>_scale (+ optional
 * in_voltage<ch>_offset), per
 * Documentation/ABI/testing/sysfs-bus-iio.
 *
 * Registered at priority 100 with vendor "linux"; the sw_fallback
 * backend (priority 0) still wins on non-Linux native_sim builds where
 * this TU compiles to an empty object.  Selected on any silicon
 * (silicon_ref "*") because the IIO ABI is SoC-agnostic; the
 * device-tree / kernel decides which physical SAR ADC backs
 * iio:deviceN.
 *
 * Status: REAL implementation; Yocto sysroot link + on-target run are
 * BENCH-UNVERIFIED (no IIO device nodes / sysroot in this tree).
 *
 * read_uv mapping note:
 *   The dispatcher's alp_adc_read_uv() computes
 *       uv = raw * state->reference_uv / (2^state->resolution_bits - 1).
 *   IIO instead expresses the result as
 *       processed_mV = (raw + offset) * scale_mV_per_count.
 *   To make the dispatcher's linear formula reproduce the IIO scale
 *   exactly regardless of the true ADC width, we pin
 *   resolution_bits = 16 (full-scale 65535) and set
 *       reference_uv = round(scale_uV_per_count * 65535).
 *   Then  raw * reference_uv / 65535 == raw * scale_uV_per_count,
 *   the offset-free IIO conversion.  A non-zero in_voltage<ch>_offset
 *   cannot be folded into the dispatcher's formula without corrupting
 *   the raw contract of read_raw(); see the open() comment.
 */

#if defined(__linux__)

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <alp/adc.h>
#include <alp/backend.h>
#include <alp/cap_instance.h>
#include <alp/peripheral.h>

#include "adc_ops.h"
#include "common/alp_errno.h"

/* The IIO sysfs channel files we read are conventionally short ASCII;
 * 64 bytes covers an integer raw code, a "<int>.<frac>" scale, or an
 * offset with room to spare. */
#define Y_ADC_SYSBUF 64

/* Pinned full-scale used to project the IIO mV/count scale onto the
 * dispatcher's raw*ref/(2^bits-1) formula.  See the file header. */
#define Y_ADC_RES_BITS  16u
#define Y_ADC_FULLSCALE 65535u /* (1u << Y_ADC_RES_BITS) - 1u */

/* Per-handle backend data: the IIO device id + channel index, the
 * cached uV-per-count scale, and the raw offset (kept for diagnostics;
 * not applied by read_raw -- see open()). */
typedef struct {
	unsigned device_id;
	unsigned channel;
	long     offset_raw; /* in_voltage<ch>_offset, raw counts; 0 if absent */
} y_adc_data_t;

/**
 * @brief Read a small IIO sysfs attribute file into @p buf.
 *
 * @return ALP_OK with a NUL-terminated, newline-stripped @p buf, or an
 *         errno-mapped status.  ENOENT (attribute absent) is left for
 *         the caller to treat as "not present" where that is valid.
 */
static alp_status_t
_read_attr(unsigned device_id, unsigned channel, const char *suffix, char *buf, size_t buflen)
{
	char path[96];
	int  n = snprintf(path,
	                  sizeof(path),
	                  "/sys/bus/iio/devices/iio:device%u/in_voltage%u_%s",
	                  device_id,
	                  channel,
	                  suffix);
	if (n < 0 || (size_t)n >= sizeof(path)) {
		return ALP_ERR_INVAL;
	}

	int fd = open(path, O_RDONLY | O_CLOEXEC);
	if (fd < 0) {
		return alp_status_from_posix_errno(errno);
	}

	ssize_t r = read(fd, buf, buflen - 1u);
	int     e = errno;
	close(fd);
	if (r < 0) {
		return alp_status_from_posix_errno(e);
	}
	buf[r] = '\0';
	/* Strip the trailing newline the kernel appends. */
	if (r > 0 && buf[r - 1] == '\n') {
		buf[r - 1] = '\0';
	}
	return ALP_OK;
}

/**
 * @brief Convert an IIO mV/count scale string to uV/count.
 *
 * IIO scale is a decimal "<int>.<frac>" in millivolts per raw count.
 * Parse it as microvolts-per-count using integer math (no float in
 * the backend) by reading up to three fractional digits -- i.e. 1 uV
 * (0.001 mV) per-count resolution, which is the precision the uV-output
 * contract needs.  Further fractional digits are below 1 uV and are
 * skipped.
 *
 * @return ALP_OK with @p uv_per_count set, or ALP_ERR_INVAL on a
 *         malformed value.
 */
static alp_status_t _scale_mv_str_to_uv(const char *s, uint64_t *uv_per_count)
{
	/* Skip leading whitespace. */
	while (*s == ' ' || *s == '\t') {
		s++;
	}
	if (*s == '\0' || *s == '-') {
		/* Negative scale is not meaningful for a single-ended voltage
         * channel and cannot be represented in reference_uv (unsigned);
         * reject rather than guess. */
		return ALP_ERR_INVAL;
	}

	uint64_t whole     = 0;
	int      saw_digit = 0;
	while (*s >= '0' && *s <= '9') {
		whole = whole * 10u + (uint64_t)(*s - '0');
		s++;
		saw_digit = 1;
	}

	uint64_t micros = whole * 1000u; /* mV -> uV */
	if (*s == '.') {
		s++;
		uint64_t scale = 100u; /* 0.1 mV = 100 uV at the first frac digit */
		/* Three digits: weights 100 -> 10 -> 1 uV.  A 4th digit would be
         * 0.1 uV, below the 1 uV resolution and (with integer 1u/10u==0)
         * would contribute nothing, so stop at three. */
		for (int i = 0; i < 3 && *s >= '0' && *s <= '9'; ++i) {
			micros += (uint64_t)(*s - '0') * scale;
			scale /= 10u;
			s++;
			saw_digit = 1;
		}
		/* Drop any remaining sub-1uV digits (below our resolution). */
		while (*s >= '0' && *s <= '9') {
			s++;
		}
	}

	if (!saw_digit) {
		return ALP_ERR_INVAL;
	}
	*uv_per_count = micros;
	return ALP_OK;
}

/**
 * @brief Resolve the IIO device + channel and cache the volt scale.
 *
 * Verifies the channel's in_voltage<ch>_raw and _scale attributes are
 * readable, derives reference_uv from the scale (see file header), and
 * pins resolution_bits to @ref Y_ADC_RES_BITS so the dispatcher's
 * read_uv math matches the IIO mV/count scale.
 *
 * channel_id is reused as the IIO device id (iio:device<id>); the IIO
 * channel index within that device is fixed at in_voltage0 because the
 * portable alp_adc config carries a single channel_id and no separate
 * device/channel split.  A SoM exposing multiple channels on one IIO
 * device is BENCH-UNVERIFIED here; the device/channel split would be a
 * vendor-ext knob, not invented in this portable path.
 *
 * Returns ALP_ERR_NOT_READY when the device/attribute is absent;
 * ALP_ERR_OUT_OF_RANGE if cfg requests a resolution exceeding the
 * pinned mapping width; ALP_ERR_NOMEM on the per-handle box.
 */
static alp_status_t
y_open(const alp_adc_config_t *cfg, alp_adc_backend_state_t *st, alp_capabilities_t *caps_out)
{
	if (cfg == NULL || st == NULL || caps_out == NULL) {
		return ALP_ERR_INVAL;
	}
	/* A caller-pinned resolution above our projection width can't be
     * honoured through the dispatcher's fixed-width read_uv formula. */
	if (cfg->resolution_bits != 0 && cfg->resolution_bits > Y_ADC_RES_BITS) {
		return ALP_ERR_OUT_OF_RANGE;
	}

	const unsigned device_id = (unsigned)cfg->channel_id;
	const unsigned channel   = 0u; /* see doxygen above */

	/* Probe the raw attribute to confirm the channel exists + is
     * readable; the value itself is discarded here. */
	char         rawbuf[Y_ADC_SYSBUF];
	alp_status_t rc = _read_attr(device_id, channel, "raw", rawbuf, sizeof(rawbuf));
	if (rc != ALP_OK) {
		return rc;
	}

	/* Scale (mV per count) -> uV per count. */
	char scalebuf[Y_ADC_SYSBUF];
	rc = _read_attr(device_id, channel, "scale", scalebuf, sizeof(scalebuf));
	if (rc != ALP_OK) {
		return rc;
	}
	uint64_t uv_per_count = 0;
	rc                    = _scale_mv_str_to_uv(scalebuf, &uv_per_count);
	if (rc != ALP_OK) {
		return rc;
	}

	/* Optional raw offset.  IIO's processed value is
     * (raw + offset) * scale; the dispatcher's read_uv has no offset
     * term and read_raw must report the true raw code, so a non-zero
     * offset is cached for diagnostics only and NOT applied.  Channels
     * needing offset correction are BENCH-UNVERIFIED here. */
	long offset_raw = 0;
	char offbuf[Y_ADC_SYSBUF];
	if (_read_attr(device_id, channel, "offset", offbuf, sizeof(offbuf)) == ALP_OK) {
		offset_raw = strtol(offbuf, NULL, 10);
	}

	/* reference_uv = uv_per_count * full-scale.  Saturate at UINT32_MAX
     * (a >65 V single-ended SAR reference is not a real Alp target;
     * guard the multiply rather than overflow). */
	uint64_t ref = uv_per_count * (uint64_t)Y_ADC_FULLSCALE;
	if (ref > (uint64_t)UINT32_MAX) {
		ref = (uint64_t)UINT32_MAX;
	}

	y_adc_data_t *d = (y_adc_data_t *)malloc(sizeof(*d));
	if (d == NULL) {
		return ALP_ERR_NOMEM;
	}
	d->device_id  = device_id;
	d->channel    = channel;
	d->offset_raw = offset_raw;

	st->be_data         = d;
	st->reference_uv    = (uint32_t)ref;
	st->resolution_bits = (uint16_t)Y_ADC_RES_BITS;

	caps_out->flags               = 0u;
	caps_out->max_resolution_bits = (uint16_t)Y_ADC_RES_BITS;
	caps_out->max_sample_rate     = 0u; /* one-shot sysfs read; rate not advertised */
	caps_out->channel_count       = 1u;
	return ALP_OK;
}

/**
 * @brief One-shot read of in_voltage<ch>_raw.
 *
 * Returns the ADC's native code as reported by the kernel driver.  No
 * scale/offset is applied here (that is the dispatcher's read_uv job),
 * keeping the raw contract intact.
 */
static alp_status_t y_read_raw(alp_adc_backend_state_t *st, int32_t *raw_out)
{
	if (st == NULL || raw_out == NULL) {
		return ALP_ERR_INVAL;
	}
	y_adc_data_t *d = (y_adc_data_t *)st->be_data;
	if (d == NULL) {
		return ALP_ERR_NOT_READY;
	}

	char         buf[Y_ADC_SYSBUF];
	alp_status_t rc = _read_attr(d->device_id, d->channel, "raw", buf, sizeof(buf));
	if (rc != ALP_OK) {
		return rc;
	}

	errno     = 0;
	char *end = NULL;
	long  v   = strtol(buf, &end, 10);
	if (end == buf || errno == ERANGE) {
		return ALP_ERR_IO;
	}
	*raw_out = (int32_t)v;
	return ALP_OK;
}

/** @brief Free the per-handle box. */
static void y_close(alp_adc_backend_state_t *st)
{
	if (st == NULL) {
		return;
	}
	y_adc_data_t *d = (y_adc_data_t *)st->be_data;
	if (d != NULL) {
		free(d);
		st->be_data = NULL;
	}
}

static const alp_adc_ops_t _ops = {
	.open     = y_open,
	.read_raw = y_read_raw,
	.close    = y_close,
};

ALP_BACKEND_REGISTER(adc,
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
