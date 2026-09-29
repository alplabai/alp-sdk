/* src/render/sprite.h */
#ifndef TR_SPRITE_H
#define TR_SPRITE_H

#include <stdint.h>

/*
 * A sprite: w x h pixels, 4-bit palette indices against a shared 16-entry
 * palette, two indices per byte, high nibble first (hi = the even-x pixel,
 * lo = the odd-x pixel next to it). ROW-ALIGNED: each row starts its own
 * fresh byte -- row stride is ceil(w/2) bytes -- so an odd width leaves that
 * row's last byte half used (low nibble 0, unused) rather than spilling into
 * the next row. This is the one convention both tools/mkatlas.py (the
 * writer) and sprite.c (the reader) must agree on; task-11-review.md finding
 * 2 is what a silent disagreement here looks like (a shear, not a crash).
 * Index 0 means transparent -- tr_sprite_draw() skips it -- which is the
 * whole of the alpha story here; there is no separate mask. See
 * tools/mkatlas.py for the encoder and src/render/atlas.h for the data this
 * project actually draws.
 */
typedef struct {
	uint16_t       w, h;
	const uint8_t *px; /* 4-bit indices, two per byte, high nibble first. */
} tr_sprite_t;

/*
 * Point sprite drawing at a target: `buf` is `frame_w` x `frame_h` RGB565
 * pixels, row-major, and `palette` is the 16-entry RGB565 table every
 * sprite's indices are resolved against (one shared palette for the whole
 * atlas). Must be called before tr_sprite_draw(). This file has no
 * Zephyr/alp-sdk dependency and does not touch a display itself -- the
 * caller (render.c) owns the buffer, the palette, and the actual blit.
 */
void tr_sprite_set_target(uint16_t *buf, uint16_t frame_w, uint16_t frame_h, const uint16_t palette[16]);

/*
 * Draw sprite `s` with its top-left at (x, y) in the target set above,
 * clipped against the target's four edges -- a sprite (or part of one)
 * outside [0, frame_w) x [0, frame_h) is simply not written, never out of
 * bounds. Index 0 is transparent and is skipped, never painted as palette
 * colour 0. Integer only.
 */
void tr_sprite_draw(int16_t x, int16_t y, const tr_sprite_t *s);

/*
 * Stateless form of tr_sprite_draw() for a strided framebuffer (the A32 HUD
 * blit straight into the NC framebuffer after the bands): `fb` is fb_w x
 * fb_h RGB565 with row stride `stride_px`, `palette` the 16-entry table.
 * Same clipping and index-0 transparency as tr_sprite_draw() (which is now
 * this with stride == frame_w).
 */
void tr_sprite_blit(uint16_t *fb, uint32_t stride_px, uint16_t fb_w, uint16_t fb_h, int32_t x, int32_t y,
		    const tr_sprite_t *s, const uint16_t palette[16]);

/*
 * Score HUD support (F7, whole-branch review). Pure decimal decomposition,
 * no sprite/display knowledge -- kept here (host-compiled, see
 * tests/host/test_atlas.c) rather than in render.c specifically so it is
 * testable: render.c is not host-compiled, which is exactly how the
 * collision-vs-draw bug (task-11-review.md finding 12) sat undetected
 * across two completed tasks.
 */
#define TR_SCORE_MAX_DIGITS 5 /* Up to 99999; see tr_score_to_digits()'s saturation behaviour. */

/*
 * Decomposes `score` into decimal digits, most-significant digit first,
 * into `out[0..return value - 1]`. Returns the digit count, always 1..
 * TR_SCORE_MAX_DIGITS (score 0 -> one digit, "0", never zero digits).
 * A `score` that would need more than TR_SCORE_MAX_DIGITS digits SATURATES
 * to the largest displayable value (all 9s) rather than silently
 * truncating to its low digits, which would show a materially wrong,
 * smaller number instead of an honestly-capped one.
 */
int tr_score_to_digits(uint32_t score, uint8_t out[TR_SCORE_MAX_DIGITS]);

#endif /* TR_SPRITE_H */
