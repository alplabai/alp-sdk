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
 * TWO DOMAINS, separated by SIGN.  alp_status_from_posix_errno() takes a
 * POSITIVE POSIX errno; alp_status_from_zephyr_errno() takes the NEGATIVE
 * value the Zephyr driver APIs return.  Passing one domain's value to the
 * other's mapper is a caller bug, and each mapper is written so that
 * mistake lands on ALP_ERR_IO rather than accidentally matching an arm.
 *
 * The Zephyr twin used to be described here as "future work tracked by
 * #630's Zephyr-side follow-up".  In its absence 27 Zephyr backends each
 * hand-rolled the switch and drifted exactly as the 31 POSIX copies above
 * had: -EAGAIN reached ALP_ERR_TIMEOUT at 16 sites and ALP_ERR_IO at 11,
 * and 8 sites could not return ALP_ERR_TIMEOUT at all.  It is no longer
 * future work (#1638) -- see alp_status_from_zephyr_errno() below, and
 * read its -EAGAIN note before "harmonising" the two baselines.
 *
 * Static-inline lives in the header rather than a separate TU so
 * callers don't pay a function-call cost on the hot path (ioctl/read/
 * write returns).  The compiler de-dupes through ICF / LTO; the source
 * is the load-bearing single source of truth.
 *
 * The POSIX half is Linux-only -- every caller already gates itself on
 * CMAKE_SYSTEM_NAME == Linux / `#if defined(__linux__)`.  The Zephyr half
 * is for the Zephyr backends; `src/common` is on the Zephyr include path
 * (zephyr/CMakeLists.txt), so those TUs include this as "alp_errno.h"
 * while the Yocto ones, reached from `src/`, use "common/alp_errno.h".
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

/**
 * @brief Map a NEGATIVE Zephyr errno to an @ref alp_status_t.
 *
 * The Zephyr driver APIs return `-EXXX`; @ref alp_status_from_posix_errno
 * above takes `+EXXX`.  This is the negative-domain twin, and it negates
 * and DELEGATES to that baseline rather than restating the table, so the
 * two cannot drift apart arm by arm -- which is the whole defect being
 * closed here (#1638).
 *
 * Three arms are answered locally instead of delegating, each for a
 * stated reason:
 *
 * @par -EAGAIN is a deliberate divergence
 *      This returns @ref ALP_ERR_TIMEOUT where the POSIX baseline returns
 *      @ref ALP_ERR_BUSY.  In Zephyr driver APIs `-EAGAIN` signals an
 *      expired deadline (`i2s_read`, `spi_transceive`, `k_sem_take`-backed
 *      paths); in POSIX socket/file APIs `EAGAIN` means "would block, retry
 *      now" -- a busy signal.  Sixteen of the twenty-seven backends this
 *      replaced already answered TIMEOUT, and
 *      src/backends/can/yocto_drv.c overrides the POSIX baseline the same
 *      way, for the same reason, with the same rationale written down.  Do
 *      not "harmonise" the two baselines without reading that.
 *
 * @par -ENOSPC and -ERANGE have no baseline arm at all
 *      Both would fall through to @ref ALP_ERR_IO, but both appear in the
 *      mappers that delegate here -- `-ENOSPC` in
 *      src/backends/storage/zephyr_littlefs.c and
 *      src/backends/jpeg/alif_hantro.c, `-ERANGE` in that littlefs mapper
 *      and in src/backends/storage/zephyr_flash.c -- so delegating them
 *      would silently demote a full filesystem and an out-of-range offset
 *      to a generic I/O error.
 *
 * @param[in] err  Negative Zephyr errno, or 0.  A POSITIVE value is the
 *                 wrong domain and yields @ref ALP_ERR_IO; it is not
 *                 silently treated as POSIX.
 * @return The mapped status.
 */
static inline alp_status_t alp_status_from_zephyr_errno(int err)
{
	if (err == 0) return ALP_OK;
	if (err > 0) return ALP_ERR_IO;                  /* wrong domain -- see @param */
	if (err == -EAGAIN) return ALP_ERR_TIMEOUT;      /* see @par above */
	if (err == -ENOSPC) return ALP_ERR_NOMEM;        /* no baseline arm */
	if (err == -ERANGE) return ALP_ERR_OUT_OF_RANGE; /* no baseline arm */
	return alp_status_from_posix_errno(-err);
}

/**
 * @brief @ref alp_status_from_zephyr_errno with per-backend overrides.
 *
 * The negative-domain counterpart of @ref alp_status_from_posix_errno_ex,
 * for the backend whose driver returns an errno the shared baseline has no
 * business carrying (`-ENOBUFS` from the Hantro JPEG encoder means "the
 * caller's output buffer was too small", not a general allocation failure).
 *
 * @note Overrides are matched on the NEGATIVE value -- write
 *       `{ -ENOBUFS, ... }`, not `{ ENOBUFS, ... }`.  Each `_ex` form
 *       matches in its own domain, so a positively-keyed table here
 *       compiles and then silently never matches.
 *
 * @param[in] err          Negative Zephyr errno, or 0.
 * @param[in] overrides    Table searched before the baseline (first match
 *                         wins); may be NULL when @p n_overrides is 0.
 * @param[in] n_overrides  Entries in @p overrides.
 * @return The mapped status.
 */
static inline alp_status_t
alp_status_from_zephyr_errno_ex(int err, const alp_errno_override_t *overrides, size_t n_overrides)
{
	for (size_t i = 0; i < n_overrides; ++i) {
		if (overrides[i].err == err) return overrides[i].status;
	}
	return alp_status_from_zephyr_errno(err);
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_COMMON_ALP_ERRNO_H_ */
