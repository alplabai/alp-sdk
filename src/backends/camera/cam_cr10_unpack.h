/* SPDX-License-Identifier: Apache-2.0 */
/**
 * @file cam_cr10_unpack.h
 * @brief RZ CRU "CR10" (64-bit packed RAW10) row unpack, scalar + AArch64 NEON.
 *
 * Six 10-bit pixels live in the low 60 bits of each little-endian 64-bit
 * word (pixel k at bits [10k, 10k+10)); a row is ceil(w / 6) words.  Each
 * sample is written as (v >> shift): one byte (out_bytes == 1) or one
 * little-endian uint16 (out_bytes == 2).
 *
 * The NEON path is bit-identical to the scalar one (tests/yocto/
 * camera_cr10_unpack.c proves it across widths, shifts and both output
 * sizes).  Both read exactly ceil(w / 6) * 8 bytes of the row and nothing
 * past it, so a caller reading an uncached buffer sees no extra bus traffic.
 * Define CAM_CR10_FORCE_SCALAR to compile the NEON path out.
 */
#ifndef ALP_BACKENDS_CAMERA_CAM_CR10_UNPACK_H
#define ALP_BACKENDS_CAMERA_CAM_CR10_UNPACK_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if defined(__aarch64__) && defined(__ARM_NEON) && !defined(CAM_CR10_FORCE_SCALAR)
#include <arm_neon.h>
#define CAM_CR10_HAVE_NEON 1
#else
#define CAM_CR10_HAVE_NEON 0
#endif

/** Little-endian 64-bit load; the address need not be aligned. */
static inline uint64_t cam_ld64le(const uint8_t *p)
{
	uint64_t w;
	memcpy(&w, p, sizeof(w));
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
	w = __builtin_bswap64(w);
#endif
	return w;
}

/** @brief Portable unpack of one row of @p w pixels (the reference). */
static inline void cam_cr10_unpack_row_scalar(const uint8_t *row,
                                              uint32_t       w,
                                              uint8_t       *dst,
                                              unsigned       out_bytes,
                                              unsigned       shift)
{
	for (uint32_t x = 0; x < w;) {
		uint64_t word = cam_ld64le(row + (size_t)(x / 6u) * 8u);
		/* one 64-bit word carries up to six pixels */
		for (unsigned k = 0; k < 6u && x < w; ++k, ++x) {
			uint32_t v = (uint32_t)((word >> (k * 10u)) & 0x3FFu) >> shift;
			if (out_bytes == 1u) {
				*dst++ = (uint8_t)v;
			} else {
				*dst++ = (uint8_t)(v & 0xFFu);
				*dst++ = (uint8_t)(v >> 8);
			}
		}
	}
}

#if CAM_CR10_HAVE_NEON

/* 24 pixels = 4 words = 32 source bytes per step.  Output lane i of vector t
 * is pixel 8t+i; pixel p sits at bit 10*(p%6) of word p/6, so its 10 bits
 * always fall inside the two bytes b0, b0+1 (b0 = 8*(p/6) + 10*(p%6)/8) at a
 * right shift of 10*(p%6) % 8 (0, 2, 4 or 6).  The tables gather those byte
 * pairs; shifts are negative because vshlq_u16 shifts right on a negative count. */
static const uint8_t cam_cr10_idx[3][16] = {
	{ 0, 1, 1, 2, 2, 3, 3, 4, 5, 6, 6, 7, 8, 9, 9, 10 },
	{ 10, 11, 11, 12, 13, 14, 14, 15, 16, 17, 17, 18, 18, 19, 19, 20 },
	{ 21, 22, 22, 23, 24, 25, 25, 26, 26, 27, 27, 28, 29, 30, 30, 31 },
};
static const int16_t cam_cr10_rsh[3][8] = {
	{ 0, -2, -4, -6, 0, -2, 0, -2 },
	{ -4, -6, 0, -2, 0, -2, -4, -6 },
	{ 0, -2, 0, -2, -4, -6, 0, -2 },
};

/** @brief NEON unpack of one row of @p w pixels; same contract as the scalar one. */
static inline void cam_cr10_unpack_row_neon(const uint8_t *row,
                                            uint32_t       w,
                                            uint8_t       *dst,
                                            unsigned       out_bytes,
                                            unsigned       shift)
{
	const uint16x8_t mask = vdupq_n_u16(0x3FFu);
	const int16x8_t  post = vdupq_n_s16((int16_t)-(int)shift);
	uint8x16_t       idx[3];
	int16x8_t        rsh[3];
	for (unsigned t = 0; t < 3u; ++t) {
		idx[t] = vld1q_u8(cam_cr10_idx[t]);
		rsh[t] = vld1q_s16(cam_cr10_rsh[t]);
	}

	uint32_t x = 0;
	for (; x + 24u <= w; x += 24u) {
		uint8x16x2_t src;
		src.val[0] = vld1q_u8(row + (size_t)(x / 6u) * 8u);
		src.val[1] = vld1q_u8(row + (size_t)(x / 6u) * 8u + 16u);
		for (unsigned t = 0; t < 3u; ++t) {
			uint16x8_t v = vreinterpretq_u16_u8(vqtbl2q_u8(src, idx[t]));
			v            = vshlq_u16(v, rsh[t]);
			v            = vandq_u16(v, mask);
			v            = vshlq_u16(v, post);
			if (out_bytes == 1u) {
				vst1_u8(dst + (size_t)(x + 8u * t), vmovn_u16(v));
			} else {
				vst1q_u8(dst + 2u * (size_t)(x + 8u * t), vreinterpretq_u8_u16(v));
			}
		}
	}
	/* x is a multiple of 24, so a multiple of 6: the tail restarts on a word edge */
	if (x < w) {
		cam_cr10_unpack_row_scalar(
		    row + (size_t)(x / 6u) * 8u, w - x, dst + (size_t)x * out_bytes, out_bytes, shift);
	}
}

#endif /* CAM_CR10_HAVE_NEON */

/** @brief Unpack one row of @p w pixels: NEON where built in, else scalar. */
static inline void cam_cr10_unpack_row(const uint8_t *row,
                                       uint32_t       w,
                                       uint8_t       *dst,
                                       unsigned       out_bytes,
                                       unsigned       shift)
{
#if CAM_CR10_HAVE_NEON
	cam_cr10_unpack_row_neon(row, w, dst, out_bytes, shift);
#else
	cam_cr10_unpack_row_scalar(row, w, dst, out_bytes, shift);
#endif
}

#endif /* ALP_BACKENDS_CAMERA_CAM_CR10_UNPACK_H */
