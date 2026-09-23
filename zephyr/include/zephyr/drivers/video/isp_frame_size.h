/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Byte geometry (default pitch, full frame size) for an ISP-Pico output
 * video_format. The ONE place zephyr/drivers/video/isp_pico.c (isp_set_fmt()'s
 * pitch fill-in, isp_dequeue()'s bytesused) AND
 * src/backends/camera/alif_isp_pico.c (its buffer-pool allocation size)
 * derive a format's size from -- so a future edit can only get this right or
 * wrong once, not drift into two independently-wrong copies of the same
 * formula. Also included directly by tests/unit/isp_frame_size on
 * native_sim: needs only video_bits_per_pixel() and the VIDEO_PIX_FMT_*
 * FOURCCs from video_alif.h, no DT/MMIO/real ISP.
 *
 * Silicon-proven bug this exists to fix (E1M-AEN803, bench run 200): the ISP
 * MI output negotiates with pitch == 0 (alif_isp_pico.c's
 * _negotiate_output_fmt()); isp_pico.c never filled it in, and separately
 * sized bytesused as pitch*height -- 0 for every YUV420/NV12 frame, so
 * callers copied nothing and read stale memory.
 */

#ifndef ZEPHYR_INCLUDE_DRIVERS_VIDEO_ISP_FRAME_SIZE_H_
#define ZEPHYR_INCLUDE_DRIVERS_VIDEO_ISP_FRAME_SIZE_H_

#include <stdint.h>

#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/video_alif.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bits per pixel for an ISP-Pico output fourcc.
 *
 * Wraps video_bits_per_pixel(), extended with the two Alp Lab-private
 * fourccs (video_alif.h) it doesn't know: VIDEO_PIX_FMT_YUV422P (planar
 * 4:2:2 -- matches fourcc_to_plane_size()'s 1/2 + 1/4 + 1/4 plane split,
 * zephyr/drivers/video/video_alif.c) and VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE
 * (3 equal 8-bit planes, same file's 1/3 + 1/3 + 1/3 split).
 *
 * @param pixelformat FourCC pixel format value.
 * @return Bits per pixel, or 0 if the format is unknown or variable-size.
 */
static inline unsigned int alp_isp_pixel_bpp(uint32_t pixelformat)
{
	unsigned int bpp = video_bits_per_pixel(pixelformat);

	if (bpp != 0u) {
		return bpp;
	}

	switch (pixelformat) {
	case VIDEO_PIX_FMT_YUV422P:
		return 16u;
	case VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE:
		return 24u;
	default:
		return 0u;
	}
}

/**
 * @brief Default output pitch (line stride, in bytes) for a format that
 * hasn't negotiated one of its own.
 *
 * For planar/semi-planar YUV (video.h's VIDEO_FMT_IS_FULL_PLANAR /
 * VIDEO_FMT_IS_SEMI_PLANAR -- YUV420/YVU420/NV12/NV21/NV16/NV61/NV24/NV42)
 * the pitch is the LUMA-only line stride: one byte per pixel, never
 * alp_isp_pixel_bpp()'s chroma-subsampled AVERAGE (e.g. 12 for 4:2:0),
 * which isn't a whole number of bytes per pixel and isn't what any
 * consumer strides the Y plane by. Every other fourcc (packed YUV,
 * YUV422P, RGB888_PLANAR_PRIVATE, ...) uses bpp * width / 8.
 *
 * @param pixelformat FourCC pixel format value.
 * @param width Frame width in pixels.
 * @return Pitch in bytes, or 0 if the format is unknown or variable-size.
 */
static inline uint32_t alp_isp_default_pitch(uint32_t pixelformat, uint16_t width)
{
	if (VIDEO_FMT_IS_FULL_PLANAR(pixelformat) || VIDEO_FMT_IS_SEMI_PLANAR(pixelformat)) {
		return width;
	}

	unsigned int bpp = alp_isp_pixel_bpp(pixelformat);

	if (bpp == 0u) {
		return 0u;
	}

	return ((uint32_t)bpp * width) >> 3;
}

/**
 * @brief Full uncompressed frame size, in bytes, chroma planes included.
 *
 * NOT pitch * height: alp_isp_default_pitch() above is the LUMA-only
 * stride for planar/semi-planar YUV, so pitch * height would cover the Y
 * plane alone. This uses alp_isp_pixel_bpp() directly, which already
 * reports the format's TRUE average bits/pixel including chroma, so it is
 * correct for every fourcc supported_output_fmts[] (isp_pico.c) advertises.
 *
 * @param pixelformat FourCC pixel format value.
 * @param width Frame width in pixels.
 * @param height Frame height in pixels.
 * @return Frame size in bytes, or 0 if the format is unknown or variable-size.
 */
static inline uint32_t alp_isp_frame_size(uint32_t pixelformat, uint16_t width, uint16_t height)
{
	unsigned int bpp = alp_isp_pixel_bpp(pixelformat);

	if (bpp == 0u) {
		return 0u;
	}

	return ((uint32_t)width * height * bpp) / 8u;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VIDEO_ISP_FRAME_SIZE_H_ */
