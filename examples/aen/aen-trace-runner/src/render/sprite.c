/* src/render/sprite.c -- deliberately free of Zephyr and alp-sdk headers so
 * it compiles and runs on the host; see tests/host/test_atlas.c. The actual
 * display blit stays in render.c. */
#include <stddef.h> /* NULL */

#include "sprite.h"

static uint16_t       *g_buf;
static uint16_t        g_fw, g_fh;
static const uint16_t *g_pal;

void tr_sprite_set_target(uint16_t      *buf,
                          uint16_t       frame_w,
                          uint16_t       frame_h,
                          const uint16_t palette[16])
{
	g_buf = buf;
	g_fw  = frame_w;
	g_fh  = frame_h;
	g_pal = palette;
}

/*
 * Pixel (sx, sy) of sprite s, as a 4-bit palette index. Row-aligned to match
 * tools/mkatlas.py's writer: each row is its own byte run (stride
 * ceil(w/2) bytes), not a flat w*h nibble stream -- see sprite.h's struct
 * comment. Getting this wrong doesn't crash (the writer always emits at
 * least as many bytes as this can address); it shears the image one nibble
 * further per row, silently.
 */
static uint8_t sprite_index(const tr_sprite_t *s, uint16_t sx, uint16_t sy)
{
	uint32_t row_stride = ((uint32_t)s->w + 1u) / 2u; /* bytes per row */
	uint32_t byte_i     = (uint32_t)sy * row_stride + (sx / 2u);
	uint8_t  byte       = s->px[byte_i];

	return (sx & 1u) ? (byte & 0x0Fu) : (byte >> 4);
}

void tr_sprite_blit(uint16_t          *fb,
                    uint32_t           stride_px,
                    uint16_t           fb_w,
                    uint16_t           fb_h,
                    int32_t            x,
                    int32_t            y,
                    const tr_sprite_t *s,
                    const uint16_t     palette[16])
{
	/* Clip once per sprite, not per pixel: source ranges [sx0, sx1) x
	 * [sy0, sy1) that land inside [0, fb_w) x [0, fb_h). */
	int32_t sx0 = x < 0 ? -x : 0, sy0 = y < 0 ? -y : 0;
	int32_t sx1 = (int32_t)fb_w - x, sy1 = (int32_t)fb_h - y;

	sx1 = sx1 < (int32_t)s->w ? sx1 : (int32_t)s->w;
	sy1 = sy1 < (int32_t)s->h ? sy1 : (int32_t)s->h;

	for (int32_t sy = sy0; sy < sy1; sy++) {
		uint16_t *row = &fb[(uint32_t)(y + sy) * stride_px];

		for (int32_t sx = sx0; sx < sx1; sx++) {
			uint8_t idx = sprite_index(s, (uint16_t)sx, (uint16_t)sy);

			if (idx != 0) { /* Transparent: never painted as palette colour 0. */
				row[x + sx] = palette[idx];
			}
		}
	}
}

void tr_sprite_draw(int16_t x, int16_t y, const tr_sprite_t *s)
{
	if (g_buf == NULL) {
		return; /* tr_sprite_set_target() never called: nothing to draw into. */
	}
	tr_sprite_blit(g_buf, g_fw, g_fw, g_fh, x, y, s, g_pal);
}

int tr_score_to_digits(uint32_t score, uint8_t out[TR_SCORE_MAX_DIGITS])
{
	uint32_t max_val = 1;

	for (int i = 0; i < TR_SCORE_MAX_DIGITS; i++) {
		max_val *= 10u;
	}
	max_val -= 1u; /* e.g. TR_SCORE_MAX_DIGITS=5 -> 99999 */

	if (score > max_val) {
		score = max_val; /* saturate -- an honest cap, not a truncated smaller number */
	}

	uint8_t tmp[TR_SCORE_MAX_DIGITS];
	int     count = 0;

	do {
		tmp[count] = (uint8_t)(score % 10u);
		count++;
		score /= 10u;
	} while (score > 0u && count < TR_SCORE_MAX_DIGITS);

	/* tmp is least-significant-digit-first; out is most-significant-first. */
	for (int i = 0; i < count; i++) {
		out[i] = tmp[count - 1 - i];
	}
	return count;
}
