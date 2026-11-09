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

#endif /* ALP_GST_CONVERT_H */
