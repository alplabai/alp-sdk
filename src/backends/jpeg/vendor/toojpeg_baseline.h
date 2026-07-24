// //////////////////////////////////////////////////////////
// toojpeg_baseline.h
// vendored + ported to C from TooJpeg, written by Stephan Brumme, 2018-2019
// see https://create.stephan-brumme.com/toojpeg/
//
// zlib License
//
// Copyright (c) 2011-2016 Stephan Brumme
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would be
//    appreciated but is not required.
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
// 3. This notice may not be removed or altered from any source
//    distribution.
//
// -------------------------------------------------------------------------
// Provenance: github.com/stbrumme/toojpeg, commit 341de5c295e41e292fe5933e
// 7720a522b0caf1c1 ("version 1.5", 2019-07-08), files toojpeg.h/toojpeg.cpp.
//
// Vendored, THEN ALTERED (zlib clause 2 -- this is plainly marked as such,
// not the original):
//   - ported from C++ to C (namespaces/references/templates/operator<<
//     replaced with plain functions and a tj_writer_t struct; no other
//     algorithmic change to the DCT / quantization / Huffman core).
//   - entry point renamed writeJpeg() -> toojpeg_encode_yuv420().
//   - input pipeline replaced: upstream takes packed RGB/gray and does the
//     RGB->YCbCr conversion (+ optional on-the-fly 4:2:0 downsample) itself;
//     this port takes already-planar Y/Cb/Cr (the Alp SDK's <alp/jpeg.h>
//     request is always planar YCbCr, 4:2:0 or 4:0:0) and feeds the DCT
//     directly off the caller's planes with border-replication clamping --
//     no RGB round-trip, no upstream color-conversion / downsample code
//     carried over.
//   - output pipeline replaced: upstream's byte-at-a-time WRITE_ONE_BYTE
//     callback replaced with a bounded destination buffer + explicit
//     capacity check (tj_writer_t.overflow); see the entry-point overflow
//     contract below. No dynamic allocation either way.
//   - dropped the optional JPEG COM-marker comment parameter (unused by
//     the SDK's <alp/jpeg.h> request).
// //////////////////////////////////////////////////////////

#ifndef ALP_JPEG_VENDOR_TOOJPEG_BASELINE_H
#define ALP_JPEG_VENDOR_TOOJPEG_BASELINE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Encode a planar YCbCr frame (4:2:0, or 4:0:0/mono when @p mono is
 * non-zero) to a baseline-sequential JFIF/JPEG byte stream.
 *
 * @p u / @p v are ignored when @p mono is non-zero and MUST be non-NULL
 * otherwise.  Chroma planes are read at their own resolution (ceil(width/2)
 * x ceil(height/2)) and border-replicated at the right/bottom edge exactly
 * like the luma plane -- callers must NOT pre-upsample or pre-average.
 *
 * @return Bytes written to @p dst on success.
 *         0 on invalid parameters (NULL dst/y, zero width/height, NULL
 *         u or v when @p mono is zero).
 *         (size_t)-1 if @p dst_cap was too small to hold the encoded
 *         stream -- @p dst is left partially written and MUST be treated
 *         as garbage; retry with a bigger buffer, there is no partial
 *         result to salvage.
 */
size_t toojpeg_encode_yuv420(void          *dst,
                             size_t         dst_cap,
                             const uint8_t *y,
                             uint32_t       y_stride,
                             const uint8_t *u,
                             uint32_t       u_stride,
                             const uint8_t *v,
                             uint32_t       v_stride,
                             uint16_t       width,
                             uint16_t       height,
                             int            mono,
                             int            quality);

#ifdef __cplusplus
}
#endif

#endif /* ALP_JPEG_VENDOR_TOOJPEG_BASELINE_H */
