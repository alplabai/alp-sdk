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
 * width*height (missing the U/V planes) once a caller filled it in.
 *
 * Includes the SAME header isp_pico.c's isp_set_fmt()/isp_dequeue() and
 * alif_isp_pico.c's buffer sizing call -- not a stand-in copy of the
 * formula -- so reverting either helper's body here is what a reverted
 * driver/backend call site would actually run.
 */
#include <zephyr/drivers/video.h>
#include <zephyr/drivers/video/isp_frame_size.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(isp_frame_size, NULL, NULL, NULL, NULL, NULL);

/* --- alp_isp_frame_size(): full frame size, chroma included -------------- */

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

/* video_bits_per_pixel() reports 0 for these two Alp Lab-private fourccs
 * (video_alif.h) -- alp_isp_pixel_bpp()'s fallback must size them the way
 * fourcc_to_plane_size() (video_alif.c) splits their planes: YUV422P is
 * 1/2 + 1/4 + 1/4 of the buffer (16 bpp average), RGB888_PLANAR_PRIVATE is
 * three equal 1/3 planes (24 bpp). Without the fallback these fell through
 * to bytesused/pitch == 0, same class of bug as the YUV420/NV12 case. */
ZTEST(isp_frame_size, test_yuv422p_uses_plane_split_bpp)
{
	uint32_t got = alp_isp_frame_size(VIDEO_PIX_FMT_YUV422P, 640u, 480u);

	zassert_equal(
	    got, 640u * 480u * 2u, "YUV422P 640x480 bytesused: got %u, want %u", got, 640u * 480u * 2u);
	zassert_not_equal(got, 0u, "YUV422P bytesused must never be 0");
}

ZTEST(isp_frame_size, test_rgb888_planar_private_uses_plane_split_bpp)
{
	uint32_t got = alp_isp_frame_size(VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE, 640u, 480u);

	zassert_equal(got,
	              640u * 480u * 3u,
	              "RGB888_PLANAR_PRIVATE 640x480 bytesused: got %u, want %u",
	              got,
	              640u * 480u * 3u);
	zassert_not_equal(got, 0u, "RGB888_PLANAR_PRIVATE bytesused must never be 0");
}

/* --- alp_isp_default_pitch(): isp_set_fmt()'s pitch fill-in -------------- */

ZTEST(isp_frame_size, test_pitch_yuv420_is_luma_stride)
{
	uint32_t got = alp_isp_default_pitch(VIDEO_PIX_FMT_YUV420, 640u);

	/* Luma-only: one byte per pixel, NOT video_bits_per_pixel()'s 12-bpp
	 * chroma-subsampled average (which would give a non-integral
	 * 640*1.5 if it were ever (wrongly) applied per-line). */
	zassert_equal(got, 640u, "YUV420 pitch: got %u, want 640 (luma stride)", got);
}

ZTEST(isp_frame_size, test_pitch_nv12_is_luma_stride)
{
	uint32_t got = alp_isp_default_pitch(VIDEO_PIX_FMT_NV12, 640u);

	zassert_equal(got, 640u, "NV12 pitch: got %u, want 640 (luma stride)", got);
}

ZTEST(isp_frame_size, test_pitch_rgb565_is_bpp_derived)
{
	uint32_t got = alp_isp_default_pitch(VIDEO_PIX_FMT_RGB565, 640u);

	zassert_equal(got, 1280u, "RGB565 pitch: got %u, want 1280 (2 B/px)", got);
}

ZTEST(isp_frame_size, test_pitch_yuv422p_is_luma_stride)
{
	/* YUV422P is planar too (fourcc_to_plane_size(), video_alif.c, gives
	 * its plane 0 exactly `width` bytes/line) -- luma-only, like
	 * YUV420/NV12 above, not the 16-bpp average. */
	uint32_t got = alp_isp_default_pitch(VIDEO_PIX_FMT_YUV422P, 640u);

	zassert_equal(got, 640u, "YUV422P pitch: got %u, want 640 (luma stride)", got);
}

ZTEST(isp_frame_size, test_pitch_rgb888_planar_private_is_bpp_derived)
{
	uint32_t got = alp_isp_default_pitch(VIDEO_PIX_FMT_RGB888_PLANAR_PRIVATE, 640u);

	zassert_equal(got, 1920u, "RGB888_PLANAR_PRIVATE pitch: got %u, want 1920 (24 bpp)", got);
}
