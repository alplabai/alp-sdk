// //////////////////////////////////////////////////////////
// toojpeg_baseline.c
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
// Vendored, THEN ALTERED -- see the modification list in toojpeg_baseline.h.
// The quantization tables (JPEG Standard Annex K), zig-zag map, Huffman
// bit-length/value tables (JPEG Standard Annex K) and the AAN fast-DCT
// (Arai, Agui, Nakajima) are carried over unmodified; everything around
// them (language, input plumbing, output plumbing) is rewritten to C.
// //////////////////////////////////////////////////////////

#include "toojpeg_baseline.h"

/* ---- constant tables (JPEG Standard Annex K), verbatim from upstream --- */

static const uint8_t TJ_QUANT_LUMA[8 * 8] = { 16,  11,  10,  16, 24,  40,  51,  61,  12,  12,  14,
	                                          19,  26,  58,  60, 55,  14,  13,  16,  24,  40,  57,
	                                          69,  56,  14,  17, 22,  29,  51,  87,  80,  62,  18,
	                                          22,  37,  56,  68, 109, 103, 77,  24,  35,  55,  64,
	                                          81,  104, 113, 92, 49,  64,  78,  87,  103, 121, 120,
	                                          101, 72,  92,  95, 98,  112, 100, 103, 99 };
static const uint8_t TJ_QUANT_CHROMA[8 * 8] = { 17, 18, 24, 47, 99, 99, 99, 99, 18, 21, 26, 66, 99,
	                                            99, 99, 99, 24, 26, 56, 99, 99, 99, 99, 99, 47, 66,
	                                            99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	                                            99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99,
	                                            99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99, 99 };

static const uint8_t TJ_ZIGZAG_INV[8 * 8] = { 0,  1,  8,  16, 9,  2,  3,  10, 17, 24, 32, 25, 18,
	                                          11, 4,  5,  12, 19, 26, 33, 40, 48, 41, 34, 27, 20,
	                                          13, 6,  7,  14, 21, 28, 35, 42, 49, 56, 57, 50, 43,
	                                          36, 29, 22, 15, 23, 30, 37, 44, 51, 58, 59, 52, 45,
	                                          38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63 };

static const uint8_t TJ_DC_LUMA_BITS[16]    = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static const uint8_t TJ_DC_LUMA_VALUES[12]  = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t TJ_AC_LUMA_BITS[16]    = { 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 125 };
static const uint8_t TJ_AC_LUMA_VALUES[162] = {
	0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61,
	0x07, 0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xA1, 0x08, 0x23, 0x42, 0xB1, 0xC1, 0x15, 0x52,
	0xD1, 0xF0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0A, 0x16, 0x17, 0x18, 0x19, 0x1A, 0x25,
	0x26, 0x27, 0x28, 0x29, 0x2A, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44, 0x45,
	0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63, 0x64,
	0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A, 0x83,
	0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98, 0x99,
	0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6,
	0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA, 0xD2, 0xD3,
	0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE1, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7, 0xE8,
	0xE9, 0xEA, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
};
static const uint8_t TJ_DC_CHROMA_BITS[16]   = { 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0 };
static const uint8_t TJ_DC_CHROMA_VALUES[12] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11 };
static const uint8_t TJ_AC_CHROMA_BITS[16]   = { 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 119 };
static const uint8_t TJ_AC_CHROMA_VALUES[162] = {
	0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61,
	0x71, 0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91, 0xA1, 0xB1, 0xC1, 0x09, 0x23, 0x33,
	0x52, 0xF0, 0x15, 0x62, 0x72, 0xD1, 0x0A, 0x16, 0x24, 0x34, 0xE1, 0x25, 0xF1, 0x17, 0x18,
	0x19, 0x1A, 0x26, 0x27, 0x28, 0x29, 0x2A, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3A, 0x43, 0x44,
	0x45, 0x46, 0x47, 0x48, 0x49, 0x4A, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5A, 0x63,
	0x64, 0x65, 0x66, 0x67, 0x68, 0x69, 0x6A, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7A,
	0x82, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8A, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
	0x98, 0x99, 0x9A, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6, 0xA7, 0xA8, 0xA9, 0xAA, 0xB2, 0xB3, 0xB4,
	0xB5, 0xB6, 0xB7, 0xB8, 0xB9, 0xBA, 0xC2, 0xC3, 0xC4, 0xC5, 0xC6, 0xC7, 0xC8, 0xC9, 0xCA,
	0xD2, 0xD3, 0xD4, 0xD5, 0xD6, 0xD7, 0xD8, 0xD9, 0xDA, 0xE2, 0xE3, 0xE4, 0xE5, 0xE6, 0xE7,
	0xE8, 0xE9, 0xEA, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
};

#define TJ_CODEWORD_LIMIT 2048 /* +/-2^11, maximum value after DCT. */

/* ---- Huffman code + bit-level writer ------------------------------------ */

typedef struct {
	uint16_t code;
	uint8_t  num_bits;
} tj_code_t;

typedef struct {
	uint8_t *dst;
	size_t   cap;
	size_t   pos;
	int      overflow;
	int32_t  bitbuf;
	uint8_t  bitcount;
} tj_writer_t;

static void tj_put_byte(tj_writer_t *w, uint8_t b)
{
	if (w->overflow) {
		return;
	}
	if (w->pos >= w->cap) {
		w->overflow = 1;
		return;
	}
	w->dst[w->pos++] = b;
}

static void tj_put_bytes(tj_writer_t *w, const uint8_t *src, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		tj_put_byte(w, src[i]);
	}
}

/* Write Huffman bits, keeping excess bits in the bit buffer (mirrors
 * upstream BitWriter::operator<<(BitCode)). */
static void tj_put_bits(tj_writer_t *w, uint16_t code, uint8_t num_bits)
{
	w->bitcount = (uint8_t)(w->bitcount + num_bits);
	w->bitbuf   = (int32_t)((uint32_t)w->bitbuf << num_bits);
	w->bitbuf |= code;

	while (w->bitcount >= 8) {
		w->bitcount      = (uint8_t)(w->bitcount - 8);
		uint8_t one_byte = (uint8_t)((uint32_t)w->bitbuf >> w->bitcount);
		tj_put_byte(w, one_byte);
		if (one_byte == 0xFFu) {
			tj_put_byte(w, 0);
		}
	}
}

static void tj_add_marker(tj_writer_t *w, uint8_t id, uint16_t length)
{
	tj_put_byte(w, 0xFF);
	tj_put_byte(w, id);
	tj_put_byte(w, (uint8_t)(length >> 8));
	tj_put_byte(w, (uint8_t)(length & 0xFFu));
}

static int32_t tj_clamp(int32_t value, int32_t lo, int32_t hi)
{
	if (value <= lo) {
		return lo;
	}
	if (value >= hi) {
		return hi;
	}
	return value;
}

/* Forward DCT, one dimension (AAN fast-DCT: Arai, Agui, Nakajima).
 * stride is 1 (row pass) or 8 (column pass). */
static void tj_dct(float *block, int stride)
{
	const float sqrt_half_sqrt = 1.306562965f;
	const float inv_sqrt       = 0.707106781f;
	const float half_sqrt_sqrt = 0.382683432f;
	const float inv_sqrt_sqrt  = 0.541196100f;

	float b0 = block[0 * stride];
	float b1 = block[1 * stride];
	float b2 = block[2 * stride];
	float b3 = block[3 * stride];
	float b4 = block[4 * stride];
	float b5 = block[5 * stride];
	float b6 = block[6 * stride];
	float b7 = block[7 * stride];

	float add07 = b0 + b7, sub07 = b0 - b7;
	float add16 = b1 + b6, sub16 = b1 - b6;
	float add25 = b2 + b5, sub25 = b2 - b5;
	float add34 = b3 + b4, sub34 = b3 - b4;

	float add0347 = add07 + add34, sub07_34 = add07 - add34;
	float add1256 = add16 + add25, sub16_25 = add16 - add25;

	block[0 * stride] = add0347 + add1256;
	block[4 * stride] = add0347 - add1256;

	float z1          = (sub16_25 + sub07_34) * inv_sqrt;
	block[2 * stride] = sub07_34 + z1;
	block[6 * stride] = sub07_34 - z1;

	float sub23_45 = sub25 + sub34;
	float sub12_56 = sub16 + sub25;
	float sub01_67 = sub16 + sub07;

	float z5          = (sub23_45 - sub01_67) * half_sqrt_sqrt;
	float z2          = sub23_45 * inv_sqrt_sqrt + z5;
	float z3          = sub12_56 * inv_sqrt;
	float z4          = sub01_67 * sqrt_half_sqrt + z5;
	float z6          = sub07 + z3;
	float z7          = sub07 - z3;
	block[1 * stride] = z6 + z4;
	block[7 * stride] = z6 - z4;
	block[5 * stride] = z7 + z2;
	block[3 * stride] = z7 - z2;
}

/* Run DCT, quantize and emit one 8x8 block's Huffman bit codes. */
static int16_t tj_encode_block(tj_writer_t     *w,
                               float            block[8][8],
                               const float     *scaled,
                               int16_t          last_dc,
                               const tj_code_t  huff_dc[256],
                               const tj_code_t  huff_ac[256],
                               const tj_code_t *codewords)
{
	float *block64 = (float *)block;

	for (int off = 0; off < 8; off++) {
		tj_dct(block64 + off * 8, 1);
	}
	for (int off = 0; off < 8; off++) {
		tj_dct(block64 + off, 8);
	}
	for (int i = 0; i < 8 * 8; i++) {
		block64[i] *= scaled[i];
	}

	int dc = (int)(block64[0] + (block64[0] >= 0.0f ? 0.5f : -0.5f));

	int     pos_nonzero = 0;
	int16_t quantized[8 * 8];
	for (int i = 1; i < 8 * 8; i++) {
		float value  = block64[TJ_ZIGZAG_INV[i]];
		quantized[i] = (int16_t)(value + (value >= 0.0f ? 0.5f : -0.5f));
		if (quantized[i] != 0) {
			pos_nonzero = i;
		}
	}

	int diff = dc - last_dc;
	if (diff == 0) {
		tj_put_bits(w, huff_dc[0x00].code, huff_dc[0x00].num_bits);
	} else {
		tj_code_t bits = codewords[diff];
		tj_put_bits(w, huff_dc[bits.num_bits].code, huff_dc[bits.num_bits].num_bits);
		tj_put_bits(w, bits.code, bits.num_bits);
	}

	int offset = 0;
	for (int i = 1; i <= pos_nonzero; i++) {
		while (quantized[i] == 0) {
			offset += 0x10;
			if (offset > 0xF0) {
				tj_put_bits(w, huff_ac[0xF0].code, huff_ac[0xF0].num_bits);
				offset = 0;
			}
			i++;
		}
		tj_code_t encoded = codewords[quantized[i]];
		tj_code_t symbol  = huff_ac[offset + encoded.num_bits];
		tj_put_bits(w, symbol.code, symbol.num_bits);
		tj_put_bits(w, encoded.code, encoded.num_bits);
		offset = 0;
	}
	if (pos_nonzero < 8 * 8 - 1) {
		tj_put_bits(w, huff_ac[0x00].code, huff_ac[0x00].num_bits);
	}
	return (int16_t)dc;
}

static void
tj_generate_huffman_table(const uint8_t num_codes[16], const uint8_t *values, tj_code_t result[256])
{
	uint16_t huffman_code = 0;
	for (int num_bits = 1; num_bits <= 16; num_bits++) {
		for (int i = 0; i < num_codes[num_bits - 1]; i++) {
			result[*values++] = (tj_code_t){ huffman_code++, (uint8_t)num_bits };
		}
		huffman_code = (uint16_t)(huffman_code << 1);
	}
}

/* ---- entry point --------------------------------------------------------- */

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
                             int            quality)
{
	if (dst == NULL || y == NULL || width == 0u || height == 0u) {
		return 0;
	}
	if (!mono && (u == NULL || v == NULL)) {
		return 0;
	}

	tj_writer_t w = { .dst = (uint8_t *)dst, .cap = dst_cap };

	int num_components = mono ? 1 : 3;
	int downsample     = !mono; /* the SDK only ever feeds already-4:2:0 or mono planes. */

	static const uint8_t header_jfif[2 + 2 + 16] = { 0xFF, 0xD8, 0xFF, 0xE0, 0, 16, 'J',
		                                             'F',  'I',  'F',  0,    1, 1,  0,
		                                             0,    1,    0,    1,    0, 0 };
	tj_put_bytes(&w, header_jfif, sizeof(header_jfif));

	/* quantization tables, scaled to quality (formula from libjpeg). */
	int q = (int)tj_clamp(quality, 1, 100);
	q     = q < 50 ? 5000 / q : 200 - q * 2;

	uint8_t quant_luma[8 * 8], quant_chroma[8 * 8];
	for (int i = 0; i < 8 * 8; i++) {
		int luma        = (TJ_QUANT_LUMA[TJ_ZIGZAG_INV[i]] * q + 50) / 100;
		int chroma      = (TJ_QUANT_CHROMA[TJ_ZIGZAG_INV[i]] * q + 50) / 100;
		quant_luma[i]   = (uint8_t)tj_clamp(luma, 1, 255);
		quant_chroma[i] = (uint8_t)tj_clamp(chroma, 1, 255);
	}

	tj_add_marker(&w, 0xDB, (uint16_t)(2 + (mono ? 1 : 2) * (1 + 8 * 8)));
	tj_put_byte(&w, 0x00);
	tj_put_bytes(&w, quant_luma, sizeof(quant_luma));
	if (!mono) {
		tj_put_byte(&w, 0x01);
		tj_put_bytes(&w, quant_chroma, sizeof(quant_chroma));
	}

	/* SOF0 -- start of frame. */
	tj_add_marker(&w, 0xC0, (uint16_t)(2 + 6 + 3 * num_components));
	tj_put_byte(&w, 0x08);
	tj_put_byte(&w, (uint8_t)(height >> 8));
	tj_put_byte(&w, (uint8_t)(height & 0xFFu));
	tj_put_byte(&w, (uint8_t)(width >> 8));
	tj_put_byte(&w, (uint8_t)(width & 0xFFu));
	tj_put_byte(&w, (uint8_t)num_components);
	for (int id = 1; id <= num_components; id++) {
		tj_put_byte(&w, (uint8_t)id);
		tj_put_byte(&w, (uint8_t)(id == 1 && downsample ? 0x22 : 0x11));
		tj_put_byte(&w, (uint8_t)(id == 1 ? 0 : 1));
	}

	/* DHT -- Huffman tables. */
	tj_add_marker(&w, 0xC4, (uint16_t)(mono ? (2 + 208) : (2 + 208 + 208)));
	tj_put_byte(&w, 0x00);
	tj_put_bytes(&w, TJ_DC_LUMA_BITS, sizeof(TJ_DC_LUMA_BITS));
	tj_put_bytes(&w, TJ_DC_LUMA_VALUES, sizeof(TJ_DC_LUMA_VALUES));
	tj_put_byte(&w, 0x10);
	tj_put_bytes(&w, TJ_AC_LUMA_BITS, sizeof(TJ_AC_LUMA_BITS));
	tj_put_bytes(&w, TJ_AC_LUMA_VALUES, sizeof(TJ_AC_LUMA_VALUES));

	tj_code_t huff_luma_dc[256] = { 0 }, huff_luma_ac[256] = { 0 };
	tj_generate_huffman_table(TJ_DC_LUMA_BITS, TJ_DC_LUMA_VALUES, huff_luma_dc);
	tj_generate_huffman_table(TJ_AC_LUMA_BITS, TJ_AC_LUMA_VALUES, huff_luma_ac);

	tj_code_t huff_chroma_dc[256] = { 0 }, huff_chroma_ac[256] = { 0 };
	if (!mono) {
		tj_put_byte(&w, 0x01);
		tj_put_bytes(&w, TJ_DC_CHROMA_BITS, sizeof(TJ_DC_CHROMA_BITS));
		tj_put_bytes(&w, TJ_DC_CHROMA_VALUES, sizeof(TJ_DC_CHROMA_VALUES));
		tj_put_byte(&w, 0x11);
		tj_put_bytes(&w, TJ_AC_CHROMA_BITS, sizeof(TJ_AC_CHROMA_BITS));
		tj_put_bytes(&w, TJ_AC_CHROMA_VALUES, sizeof(TJ_AC_CHROMA_VALUES));

		tj_generate_huffman_table(TJ_DC_CHROMA_BITS, TJ_DC_CHROMA_VALUES, huff_chroma_dc);
		tj_generate_huffman_table(TJ_AC_CHROMA_BITS, TJ_AC_CHROMA_VALUES, huff_chroma_ac);
	}

	/* SOS -- start of scan (single sequential scan, baseline). */
	tj_add_marker(&w, 0xDA, (uint16_t)(2 + 1 + 2 * num_components + 3));
	tj_put_byte(&w, (uint8_t)num_components);
	for (int id = 1; id <= num_components; id++) {
		tj_put_byte(&w, (uint8_t)id);
		tj_put_byte(&w, (uint8_t)(id == 1 ? 0x00 : 0x11));
	}
	tj_put_byte(&w, 0);
	tj_put_byte(&w, 63);
	tj_put_byte(&w, 0);

	/* AAN-scaled quantization tables. */
	float              scaled_luma[8 * 8], scaled_chroma[8 * 8];
	static const float aan_scale[8] = { 1.0f, 1.387039845f, 1.306562965f, 1.175875602f,
		                                1.0f, 0.785694958f, 0.541196100f, 0.275899379f };
	for (int i = 0; i < 8 * 8; i++) {
		int   row                       = TJ_ZIGZAG_INV[i] / 8;
		int   column                    = TJ_ZIGZAG_INV[i] % 8;
		float factor                    = 1.0f / (aan_scale[row] * aan_scale[column] * 8.0f);
		scaled_luma[TJ_ZIGZAG_INV[i]]   = factor / (float)quant_luma[i];
		scaled_chroma[TJ_ZIGZAG_INV[i]] = factor / (float)quant_chroma[i];
	}

	/* Precompute JPEG codewords for the quantized DCT range. */
	tj_code_t  codewords_array[2 * TJ_CODEWORD_LIMIT];
	tj_code_t *codewords = &codewords_array[TJ_CODEWORD_LIMIT];
	uint8_t    num_bits  = 1;
	int32_t    mask      = 1;
	for (int16_t value = 1; value < TJ_CODEWORD_LIMIT; value++) {
		if (value > mask) {
			num_bits++;
			mask = (mask << 1) | 1;
		}
		codewords[-value] = (tj_code_t){ (uint16_t)(mask - value), num_bits };
		codewords[+value] = (tj_code_t){ (uint16_t)value, num_bits };
	}

	int      max_w    = (int)width - 1;
	int      max_h    = (int)height - 1;
	uint32_t chroma_w = ((uint32_t)width + 1u) / 2u;
	uint32_t chroma_h = ((uint32_t)height + 1u) / 2u;
	int      max_cw   = chroma_w > 0u ? (int)chroma_w - 1 : 0;
	int      max_ch   = chroma_h > 0u ? (int)chroma_h - 1 : 0;

	int mcu_size = mono ? 8 : 16;

	int16_t last_y_dc = 0, last_cb_dc = 0, last_cr_dc = 0;
	float   y_block[8][8], cb_block[8][8], cr_block[8][8];

	for (int mcu_y = 0; mcu_y < (int)height; mcu_y += mcu_size) {
		for (int mcu_x = 0; mcu_x < (int)width; mcu_x += mcu_size) {
			for (int block_y = 0; block_y < mcu_size; block_y += 8) {
				for (int block_x = 0; block_x < mcu_size; block_x += 8) {
					for (int dy = 0; dy < 8; dy++) {
						int            row  = tj_clamp(mcu_y + block_y + dy, 0, max_h);
						const uint8_t *yrow = y + (size_t)row * y_stride;
						for (int dx = 0; dx < 8; dx++) {
							int col         = tj_clamp(mcu_x + block_x + dx, 0, max_w);
							y_block[dy][dx] = (float)yrow[col] - 128.0f;
						}
					}
					last_y_dc = tj_encode_block(
					    &w, y_block, scaled_luma, last_y_dc, huff_luma_dc, huff_luma_ac, codewords);
				}
			}

			if (mono) {
				continue;
			}

			int crow0 = mcu_y / 2, ccol0 = mcu_x / 2;
			for (int dy = 0; dy < 8; dy++) {
				int            row  = tj_clamp(crow0 + dy, 0, max_ch);
				const uint8_t *urow = u + (size_t)row * u_stride;
				const uint8_t *vrow = v + (size_t)row * v_stride;
				for (int dx = 0; dx < 8; dx++) {
					int col          = tj_clamp(ccol0 + dx, 0, max_cw);
					cb_block[dy][dx] = (float)urow[col] - 128.0f;
					cr_block[dy][dx] = (float)vrow[col] - 128.0f;
				}
			}
			last_cb_dc = tj_encode_block(
			    &w, cb_block, scaled_chroma, last_cb_dc, huff_chroma_dc, huff_chroma_ac, codewords);
			last_cr_dc = tj_encode_block(
			    &w, cr_block, scaled_chroma, last_cr_dc, huff_chroma_dc, huff_chroma_ac, codewords);
		}
	}

	tj_put_bits(&w, 0x7F, 7); /* flush -- fill the last byte's unused bits with 1s. */
	tj_put_byte(&w, 0xFF);
	tj_put_byte(&w, 0xD9); /* EOI marker. */

	if (w.overflow) {
		return (size_t)-1;
	}
	return w.pos;
}
