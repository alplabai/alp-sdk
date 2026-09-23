/*
 * SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2026 Alp Lab AB
 *
 * Full-frame byte size for an uncompressed video_format, shared by the
 * Alif ISP-Pico camera backend's buffer allocation (alif_isp_pico.c's
 * isp_open()) and pinned here for a host/native_sim test.
 *
 * NOT pitch * height: zephyr/drivers/video/isp_pico.c's isp_set_fmt()
 * fills a negotiated output format's pitch with the LUMA line stride for
 * 4:2:0 planar/semi-planar YUV (YUV420/YVU420/NV12/NV21) -- one byte per
 * pixel -- not video_bits_per_pixel()'s chroma-subsampled average (12 bpp).
 * pitch * height therefore covers only the Y plane; this helper uses
 * video_bits_per_pixel() directly, which already reports the true average
 * bits/pixel INCLUDING chroma, so it sizes correctly for every fourcc this
 * backend negotiates, planar or packed. Silicon-proven bug (E1M-AEN803,
 * bench run 200): every dequeued YUV420/NV12 buffer reported bytesused 0 /
 * under-allocated before this fix.
 */

#ifndef ALP_BACKENDS_CAMERA_ISP_FRAME_SIZE_H
#define ALP_BACKENDS_CAMERA_ISP_FRAME_SIZE_H

#include <stdint.h>

#include <zephyr/drivers/video.h>

static inline uint32_t alp_isp_frame_size(uint32_t pixelformat, uint16_t width, uint16_t height)
{
	return ((uint32_t)width * height * video_bits_per_pixel(pixelformat)) / 8u;
}

#endif /* ALP_BACKENDS_CAMERA_ISP_FRAME_SIZE_H */
