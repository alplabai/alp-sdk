/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file alp_gst_convert.h
 * @brief Pixel-layout conversions behind the alpcamerasrc GStreamer element.
 *
 * <alp/camera.h> already unpacks the RZ CRU "CR10" frame (see
 * src/backends/camera/cam_cr10_unpack.h): GREY8 is one byte per pixel,
 * RAW10 is one little-endian uint16 per pixel holding the 10-bit sample.
 * These helpers only reshape that into the GStreamer layouts, with the
 * destination strides GStreamer hands out (rows are padded to 4 bytes).
 * Plain C with no GStreamer include so tests/yocto/gst_alpcamerasrc_convert.c
 * can run them on any host.
 */
#ifndef ALP_GST_CONVERT_H
#define ALP_GST_CONVERT_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** GRAY8: copy @p h rows of @p w bytes into a destination of stride @p dst_stride. */
static inline void
alp_gst_gray8(const uint8_t *src, uint32_t w, uint32_t h, uint8_t *dst, size_t dst_stride)
{
	for (uint32_t y = 0; y < h; ++y)
		memcpy(dst + y * dst_stride, src + (size_t)y * w, w);
}

/**
 * GRAY16_LE from unpacked RAW10: the 10-bit sample is scaled to the full 16-bit
 * range (v << 6 | v >> 4), so a 0x3FF sample is 0xFFFF and 0 stays 0.
 */
static inline void alp_gst_raw10_to_gray16(const uint16_t *src,
                                           uint32_t        w,
                                           uint32_t        h,
                                           uint8_t        *dst,
                                           size_t          dst_stride)
{
	for (uint32_t y = 0; y < h; ++y) {
		uint8_t *d = dst + y * dst_stride;
		for (uint32_t x = 0; x < w; ++x) {
			uint32_t v = src[(size_t)y * w + x] & 0x3FFu;
			uint32_t o = (v << 6) | (v >> 4);
			*d++       = (uint8_t)(o & 0xFFu); /* little-endian */
			*d++       = (uint8_t)(o >> 8);
		}
	}
}

/**
 * NV12 from GRAY8: Y plane = the pixels, interleaved UV plane = 128 (no chroma).
 * @p w and @p h must be even.  The UV plane starts @p uv_off bytes into @p dst.
 */
static inline void alp_gst_gray8_to_nv12(const uint8_t *src,
                                         uint32_t       w,
                                         uint32_t       h,
                                         uint8_t       *dst,
                                         size_t         y_stride,
                                         size_t         uv_off,
                                         size_t         uv_stride)
{
	alp_gst_gray8(src, w, h, dst, y_stride);
	for (uint32_t y = 0; y < h / 2u; ++y)
		memset(dst + uv_off + y * uv_stride, 128, w);
}

/** Bayer orders, named by the top-left 2x2 quad as read in raster order. */
enum { ALP_GST_BAYER_RGGB, ALP_GST_BAYER_BGGR, ALP_GST_BAYER_GRBG, ALP_GST_BAYER_GBRG };

/** Parse "rggb"/"bggr"/"grbg"/"gbrg" (case-insensitive); -1 when unknown. */
static inline int alp_gst_bayer_parse(const char *s)
{
	static const char *const names[4] = { "rggb", "bggr", "grbg", "gbrg" };
	for (int i = 0; s != NULL && i < 4; ++i) {
		int k = 0;
		while (k < 4 && s[k] != '\0' && (s[k] | 0x20) == names[i][k])
			++k;
		if (k == 4 && s[4] == '\0') return i;
	}
	return -1;
}

/** Float gain to the Q8 multiplier the debayer uses (clamped to 0..16). */
static inline uint32_t alp_gst_gain_q8(double g)
{
	if (!(g > 0.0)) return 0u;
	if (g > 16.0) g = 16.0;
	return (uint32_t)(g * 256.0 + 0.5);
}

static inline uint32_t alp_gst_gain10(uint32_t v, uint32_t gq8)
{
	v = (v * gq8) >> 8;
	return v > 1023u ? 1023u : v;
}

/**
 * NV12 at half resolution from unpacked 10-bit Bayer (one uint16 per pixel,
 * tight rows of @p sw).  Every 2x2 quad is one pixel: R, G = (G1+G2)/2, B (R and
 * B scaled by the Q8 gains @p rg / @p bg, 256 = 1.0), then BT.601 limited-range
 * YUV in integers.  Chroma is averaged over each 2x2 block of output pixels.
 * The output is @p sw/2 x @p sh/2; both must be multiples of 4.  No AWB, no AE.
 */
static inline void alp_gst_bayer10_to_nv12(const uint16_t *src,
                                           uint32_t        sw,
                                           uint32_t        sh,
                                           int             order,
                                           uint32_t        rg,
                                           uint32_t        bg,
                                           uint8_t        *dst,
                                           size_t          y_stride,
                                           size_t          uv_off,
                                           size_t          uv_stride)
{
	/* quad index 0..3 = (row0,col0) (row0,col1) (row1,col0) (row1,col1) */
	static const uint8_t ri[4] = { 0, 3, 1, 2 };
	static const uint8_t bi[4] = { 3, 0, 2, 1 };
	const uint32_t       ow = sw / 2u, oh = sh / 2u;

	for (uint32_t oy = 0; oy < oh; oy += 2u) {
		uint8_t *yrow = dst + oy * y_stride;
		uint8_t *uv   = dst + uv_off + (oy / 2u) * uv_stride;
		for (uint32_t ox = 0; ox < ow; ox += 2u) {
			int32_t usum = 0, vsum = 0;
			for (uint32_t k = 0; k < 4u; ++k) {
				const uint32_t  px = ox + (k & 1u), py = oy + (k >> 1);
				const uint16_t *a = src + (size_t)(2u * py) * sw + 2u * px;
				const uint16_t *b = a + sw;
				uint32_t q[4]     = { a[0] & 0x3FFu, a[1] & 0x3FFu, b[0] & 0x3FFu, b[1] & 0x3FFu };
				uint32_t g2       = q[0] + q[1] + q[2] + q[3] - q[ri[order]] - q[bi[order]];
				int32_t  r        = (int32_t)alp_gst_gain10(q[ri[order]], rg);
				int32_t  b_       = (int32_t)alp_gst_gain10(q[bi[order]], bg);
				int32_t  g = (int32_t)g2; /* sum of two, so 11 bits: coefficients are doubled */
				/* BT.601 limited: Y 16..235, Cb/Cr 16..240; >>11 = /256 x /2 (g) x /4 (10->8 bit) */
				int32_t yy = (132 * r + 129 * g + 50 * b_ + 1024) >> 11;
				yrow[(k >> 1) * y_stride + ox + (k & 1u)] = (uint8_t)(yy + 16);
				usum += (-76 * r - 74 * g + 224 * b_ + 1024) >> 11;
				vsum += (224 * r - 94 * g - 36 * b_ + 1024) >> 11;
			}
			uv[ox]      = (uint8_t)(128 + ((usum + 2) >> 2));
			uv[ox + 1u] = (uint8_t)(128 + ((vsum + 2) >> 2));
		}
	}
}

#endif /* ALP_GST_CONVERT_H */
