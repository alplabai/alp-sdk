/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Shared errno -> alp_status_t mapping for the Yocto/Linux
 * peripheral wrappers.  Every src/yocto/peripheral_*.c TU used
 * to carry a local copy of this function; consolidating here
 * keeps the mapping consistent across wrappers (an ENOMEM from
 * I2C and from UART must both return ALP_ERR_NOMEM, not drift
 * independently).
 *
 * Static-inline lives in the header rather than a separate TU
 * so callers don't pay a function-call cost on the hot path
 * (ioctl/write/read returns).  The compiler de-dupes through
 * ICF / LTO; the source is the load-bearing single source of
 * truth.
 *
 * Linux-only.  Including this header from a non-Linux TU is a
 * configuration mistake -- src/yocto/CMakeLists.txt gates the
 * wrappers on CMAKE_SYSTEM_NAME == Linux.
 */

#ifndef ALP_YOCTO_ERRNO_H_
#define ALP_YOCTO_ERRNO_H_

#include <errno.h>

#include "alp/peripheral.h"

#ifdef __cplusplus
extern "C" {
#endif

static inline alp_status_t alp_yocto_errno_to_alp(int err)
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
	case ENOTSUP:
	case ENOSYS:
		return ALP_ERR_NOSUPPORT;
	case ENOENT:
	case ENODEV:
	case ENXIO:
		return ALP_ERR_NOT_READY;
	default:
		return ALP_ERR_IO;
	}
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* ALP_YOCTO_ERRNO_H_ */
