/*
 * Copyright (c) 2026 Alp Lab AB
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Pure helper split out of jpeg_hantro_vc9000e.c's jpeg_start_encode() so
 * the SWREG9 output-size-limit computation is host/native_sim
 * unit-testable without the jpeg0 hardware or a devicetree -- see
 * tests/unit/jpeg_hantro_output_limit.
 */
#ifndef ZEPHYR_DRIVERS_VIDEO_JPEG_HANTRO_VC9000E_LIMIT_H_
#define ZEPHYR_DRIVERS_VIDEO_JPEG_HANTRO_VC9000E_LIMIT_H_

#include <errno.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Compute the SWREG9 output-buffer size limit.
 *
 * SWREG9 bounds HW writes starting at the SWREG8 base address, which
 * jpeg_start_encode() programs as buf->buffer + header_size -- NOT at
 * buf->buffer itself. Programming the raw buffer capacity (buf_size)
 * unchanged lets the HW write up to header_size bytes past the end of the
 * buf->buffer allocation before it trips JPEG_BUFFER_FULL.
 *
 * @param buf_size Output buffer capacity (struct video_buffer.size).
 * @param header_size Configured JPEG header size
 *                     (CONFIG_VIDEO_JPEG_HANTRO_VC9000E_HEADER_SIZE).
 * @param limit_out Receives buf_size - header_size on success. Left
 *                   untouched on error.
 *
 * @return 0 on success, -ENOBUFS if buf_size is too small to hold the
 *         header (the subtraction would underflow the unsigned limit back
 *         up near UINT32_MAX, re-opening the overrun this guard exists to
 *         close).
 */
static inline int
jpeg_hantro_vc9000e_output_limit(uint32_t buf_size, uint32_t header_size, uint32_t *limit_out)
{
	if (buf_size <= header_size) {
		return -ENOBUFS;
	}
	*limit_out = buf_size - header_size;
	return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_DRIVERS_VIDEO_JPEG_HANTRO_VC9000E_LIMIT_H_ */
