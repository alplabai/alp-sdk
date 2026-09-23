/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Byte geometry (default pitch, full frame size) and MRSZ scaler-ratio math
 * for an ISP-Pico output video_format. The ONE place
 * zephyr/drivers/video/isp_pico.c (isp_set_fmt()'s pitch fill-in,
 * isp_dequeue()'s bytesused, isp_apply_mrsz()'s ISP_MRSZ_SCALE_VC) AND
 * src/backends/camera/alif_isp_pico.c (its buffer-pool allocation size)
 * derive a format's size/scale from -- so a future edit can only get this
 * right or wrong once, not drift into two independently-wrong copies of the
 * same formula. Also included directly by tests/unit/isp_frame_size on
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
 * VIDEO_FMT_IS_SEMI_PLANAR -- YUV420/YVU420/NV12/NV21/NV16/NV61/NV24/NV42 --
 * plus the Alp Lab-private VIDEO_PIX_FMT_YUV422P, which video.h's macros
 * don't know) the pitch is the LUMA-only line stride: one byte per pixel,
 * never alp_isp_pixel_bpp()'s chroma-subsampled AVERAGE (e.g. 12 for
 * 4:2:0, 16 for YUV422P), which isn't a whole number of bytes per pixel
 * and isn't what any consumer strides the Y plane by --
 * fourcc_to_plane_size() (video_alif.c) gives YUV422P's plane 0 exactly
 * `width` bytes/line too. Every other fourcc (packed YUV,
 * RGB888_PLANAR_PRIVATE, ...) uses bpp * width / 8.
 *
 * @param pixelformat FourCC pixel format value.
 * @param width Frame width in pixels.
 * @return Pitch in bytes, or 0 if the format is unknown or variable-size.
 */
static inline uint32_t alp_isp_default_pitch(uint32_t pixelformat, uint16_t width)
{
	if (VIDEO_FMT_IS_FULL_PLANAR(pixelformat) || VIDEO_FMT_IS_SEMI_PLANAR(pixelformat) ||
	    pixelformat == VIDEO_PIX_FMT_YUV422P) {
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

/**
 * @brief ISP_MRSZ_SCALE_VC ratio for the main resizer's 2:1 vertical-chroma
 * downscale (4:2:2 internal -> 4:2:0 output).
 *
 * HWRM SS17.3.4.3.169 (ISP_MRSZ_SCALE_VC) gives only the register's meaning
 * ("the vertical chrominance downscale factor, or the reciprocal of the
 * vertical chrominance upscale factor" -- a 16.16 fixed-point ratio) and no
 * formula; neither the Alif DFP nor hal_alif programs this register. The
 * ISP main resizer is rkisp1-lineage silicon (VSI/rkisp1 IP); its downscale
 * ratio is RKISP1_CIF_RSZ_SCALER_FACTOR-scaled and takes a +1 that a naive
 * floor(((out-1)<<16)/(in-1)) misses:
 *   ((len_out - 1) * RKISP1_CIF_RSZ_SCALER_FACTOR) / (len_in - 1) + 1
 *
 * Silicon-proven bug this exists to fix (E1M-AEN803, bench run 205/201):
 * without the +1, 480 -> 240 downscale gives 0x7FBB (32699), one short of
 * the 0x7FBC (32700) the scaler needs to actually emit 240 chroma lines --
 * floor(((in_lines - 1) * 0x7FBB) / 65536) + 1 == 239, so the last chroma
 * row (line 239) is never written by the resizer and every 4:2:0 JPEG/raw
 * frame ships a stale/garbage final U/V row.
 *
 * @param in_lines Chroma line count into the resizer (== the ISP core's
 *                 4:2:2 luma/chroma line count, i.e. the output frame's
 *                 full height). Must be >= 2.
 * @param out_lines Target chroma line count after the 2:1 downscale
 *                  (in_lines / 2). Must be >= 1 and < in_lines.
 * @return The 16.16 fixed-point ISP_MRSZ_SCALE_VC value.
 */
static inline uint32_t alp_isp_mrsz_scale_vc(uint16_t in_lines, uint16_t out_lines)
{
	return (((uint32_t)(out_lines - 1) * 65536U) / (uint32_t)(in_lines - 1)) + 1U;
}

#ifdef __cplusplus
}
#endif

#endif /* ZEPHYR_INCLUDE_DRIVERS_VIDEO_ISP_FRAME_SIZE_H_ */
