/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared positive-POSIX-errno -> alp_status_t baseline mapping (#630).
 *
 * Every Yocto/Linux backend (src/backends/<class>/yocto_drv.c) and the
 * direct src/yocto/peripheral_*.c wrappers used to carry its own local
 * `_errno_to_alp()` / `errno_to_alp()` copy.  31 near-identical switches
 * drifted independently: some mapped EAGAIN to ALP_ERR_BUSY, others to
 * ALP_ERR_TIMEOUT, others fell through to ALP_ERR_IO; only some mapped
 * ENOMEM; ENOTSUP/EOPNOTSUPP/ENOSYS/ENOTTY coverage varied; device-not-
 * present handling (ENOENT/ENODEV/ENXIO) was inconsistent (rtc/wdt fell
 * through to ALP_ERR_IO instead of ALP_ERR_NOT_READY).  Consolidating
 * here means an ENOMEM from I2C and from CAN both return ALP_ERR_NOMEM,
 * not drift apart, and a reviewer can tell a genuine override from
 * accidental drift because only the former still exists as local code.
 *
 * Positive-POSIX-errno domain ONLY.  Do not pass a negative Zephyr
 * errno here (that is a distinct domain sign-wise; a Zephyr baseline
 * mapper is future work tracked by #630's Zephyr-side follow-up).
 *
 * Static-inline lives in the header rather than a separate TU so
 * callers don't pay a function-call cost on the hot path (ioctl/read/
 * write returns).  The compiler de-dupes through ICF / LTO; the source
 * is the load-bearing single source of truth.
 *
 * Linux-only.  Including this header from a non-POSIX TU is a
 * configuration mistake -- every caller already gates itself on
 * CMAKE_SYSTEM_NAME == Linux / `#if defined(__linux__)`.
 */

#ifndef ALP_COMMON_ALP_ERRNO_H_
#define ALP_COMMON_ALP_ERRNO_H_

#include <errno.h>
#include <stddef.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Map a positive POSIX @p err to the reviewed baseline alp_status_t.
 *
 * One reviewed mapping for the common invalid/busy/timeout/not-ready/
 * no-memory/not-supported/I/O cases (#630).  Unknown errors fail
 * conservatively as @ref ALP_ERR_IO.
 */
static inline alp_status_t alp_status_from_posix_errno(int err)
{
	switch (err) {
	case 0:
		return ALP_OK;
	case EINVAL:
		return ALP_ERR_INVAL;
	case EBUSY:
	case EAGAIN:
		return ALP_ERR_BUSY;
	case ETIMEDOUT:
		return ALP_ERR_TIMEOUT;
	case ENOMEM:
		return ALP_ERR_NOMEM;
	case ENOENT:
	case ENODEV:
	case ENXIO:
		return ALP_ERR_NOT_READY;
	case ENOTSUP:
#if defined(EOPNOTSUPP) && (EOPNOTSUPP != ENOTSUP)
	case EOPNOTSUPP:
#endif
	case ENOSYS:
	case ENOTTY:
		return ALP_ERR_NOSUPPORT;
	default:
		return ALP_ERR_IO;
	}
}

/**
 * @brief A single explicit per-operation override: @p err maps to
 *        @p status instead of the @ref alp_status_from_posix_errno
 *        baseline.
 *
 * Some variation IS operation-specific and correct (e.g. EAGAIN meaning
 * queue-full/busy for one operation versus deadline expiry for
 * another).  An override table makes that intent explicit and reviewable,
 * instead of a whole local switch silently drifting from the baseline.
 */
typedef struct {
	int          err;
	alp_status_t status;
} alp_errno_override_t;

/**
 * @brief Map @p err through @p overrides first (first match wins), then
 *        fall back to @ref alp_status_from_posix_errno for anything the
 *        override table doesn't name.
 *
 * @param err        Positive POSIX errno value (0 == success).
 * @param overrides  Operation-specific override table, or NULL.
 * @param n_overrides Number of entries in @p overrides.
 */
static inline alp_status_t
alp_status_from_posix_errno_ex(int err, const alp_errno_override_t *overrides, size_t n_overrides)
{
	for (size_t i = 0; i < n_overrides; ++i) {
		if (overrides[i].err == err) return overrides[i].status;
	}
	return alp_status_from_posix_errno(err);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_COMMON_ALP_ERRNO_H_ */
