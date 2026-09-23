/*
 * Copyright 2026 Alp Lab AB
 * SPDX-License-Identifier: Apache-2.0
 *
 * Regression test for the E1M-AEN803 silicon bug (bench run 200): the Alif
 * ISP-Pico camera backend's buffer sizing used
 * zephyr/drivers/video/isp_pico.c's channel->output_fmt.pitch * height for
 * both the dequeued bytesused (isp_pico.c) and the pool allocation size
 * (alif_isp_pico.c). For a pass-through 4:2:0 planar/semi-planar YUV
 * request (ALP_PIXFMT_YUV420_PLANAR / ALP_PIXFMT_NV12) the negotiated
 * pitch is the LUMA-only line stride, so pitch * height covered only the Y
 * plane -- 0 bytes when pitch was left at its uninitialized 0, or
 * width*height (missing the U/V planes) once a caller filled it in. Fixed
 * by keying off video_bits_per_pixel() directly (isp_frame_size.h), which
 * reports the true average bits/pixel INCLUDING chroma.
 */
#include <zephyr/drivers/video.h>
#include <zephyr/ztest.h>

#include "isp_frame_size.h"

ZTEST_SUITE(isp_frame_size, NULL, NULL, NULL, NULL, NULL);

ZTEST(isp_frame_size, test_yuv420_full_planar_covers_chroma)
{
	uint32_t got = alp_isp_frame_size(VIDEO_PIX_FMT_YUV420, 640u, 480u);

	/* w*h*3/2, not w*h (the luma-only-pitch regression value) and not 0
	 * (the uninitialized-pitch bug before any isp_set_fmt() fix). */
	zassert_equal(got, 460800u, "YUV420 640x480 bytesused: got %u, want 460800", got);
	zassert_not_equal(got, 640u * 480u, "YUV420 bytesused must cover chroma, not luma-only");
	zassert_not_equal(got, 0u, "YUV420 bytesused must never be 0");
}

ZTEST(isp_frame_size, test_nv12_semi_planar_covers_chroma)
{
	uint32_t got = alp_isp_frame_size(VIDEO_PIX_FMT_NV12, 640u, 480u);

	zassert_equal(got, 460800u, "NV12 640x480 bytesused: got %u, want 460800", got);
	zassert_not_equal(got, 640u * 480u, "NV12 bytesused must cover chroma, not luma-only");
}

/* Packed (non-planar) formats are unaffected by the planar-pitch bug --
 * pinned here so a future edit to the shared helper can't silently break
 * the packed path while fixing the planar one. */
ZTEST(isp_frame_size, test_rgb565_packed_unaffected)
{
	uint32_t got = alp_isp_frame_size(VIDEO_PIX_FMT_RGB565, 640u, 480u);

	zassert_equal(
	    got, 640u * 480u * 2u, "RGB565 640x480 bytesused: got %u, want %u", got, 640u * 480u * 2u);
}
