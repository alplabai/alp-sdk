/* tests/host/test_atlas.c */
#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "../../src/render/sprite.h"

/*
 * Self-contained: this test crafts its own tiny sprites and palette rather
 * than depending on src/render/atlas.h's real art. That keeps it stable
 * across art regenerations and lets each case pick pixel data that exercises
 * exactly the edge it names (odd width, index 0, off-screen).
 */
#define SENTINEL 0xDEADu /* Never a real palette colour: proves "untouched". */

static uint16_t test_palette[16];

static void init_palette(void)
{
	for (int i = 0; i < 16; i++) {
		/* Distinct, non-zero, non-SENTINEL values so a wrong index is
		 * obviously wrong rather than accidentally matching. */
		test_palette[i] = (uint16_t)(0x1000u * (unsigned)i + 0x0111u);
	}
	test_palette[0] = SENTINEL; /* colour 0 must never actually be painted */
}

int main(void)
{
	init_palette();

	/* 1. A known 4-bit sprite decodes to the expected palette indices, both
	 * nibbles, including an odd width where the last byte is half used --
	 * and across at least two rows, because rows are packed ROW-ALIGNED
	 * (each row restarts on its own byte, see sprite.h's struct comment): a
	 * single row cannot distinguish that convention from a flat w*h nibble
	 * stream, since they agree for h=1 (task-11-review.md finding 2). w=3,
	 * h=2, row0 = indices 1,2,3 and row1 = indices 4,5,6: byte0 hi=1 lo=2,
	 * byte1 hi=3 lo=<unused, packed as 0> (row 0 ends here even though its
	 * last nibble is unused), byte2 hi=4 lo=5, byte3 hi=6 lo=<unused>. */
	{
		static const uint8_t px[] = { 0x12, 0x30, 0x45, 0x60 };
		tr_sprite_t          s    = { .w = 3, .h = 2, .px = px };
		uint16_t             buf[3 * 2];

		for (int i = 0; i < 3 * 2; i++) {
			buf[i] = SENTINEL;
		}
		tr_sprite_set_target(buf, 3, 2, test_palette);
		tr_sprite_draw(0, 0, &s);

		assert(buf[0] == test_palette[1]);
		assert(buf[1] == test_palette[2]);
		assert(buf[2] == test_palette[3]);
		assert(buf[3] ==
		       test_palette[4]); /* row 1, col 0 -- would read palette[0] (SENTINEL) if the
						       reader treated px[] as a flat, non-row-aligned stream */
		assert(buf[4] == test_palette[5]);
		assert(buf[5] == test_palette[6]);
	}

	/* 2. Index 0 is skipped rather than drawn as palette colour 0. */
	{
		static const uint8_t px[] = { 0x01 }; /* pixel0 = idx 0, pixel1 = idx 1 */
		tr_sprite_t          s    = { .w = 2, .h = 1, .px = px };
		uint16_t             buf[2];

		buf[0] = SENTINEL;
		buf[1] = SENTINEL;
		tr_sprite_set_target(buf, 2, 1, test_palette);
		tr_sprite_draw(0, 0, &s);

		assert(buf[0] == SENTINEL); /* untouched, never became test_palette[0] */
		assert(buf[1] == test_palette[1]);
	}

	/* 3. A sprite drawn fully inside the frame writes exactly w*h
	 * non-transparent pixels, i.e. w*h minus its transparent ones. A 4x2
	 * sprite (8 px) with two index-0 pixels should change exactly 6 cells. */
	{
		/* Row0: idx 1,2,0,3  Row1: idx 4,0,5,6 -> packed high-nibble-first. */
		static const uint8_t px[] = { 0x12, 0x03, 0x40, 0x56 };
		tr_sprite_t          s    = { .w = 4, .h = 2, .px = px };
		uint16_t             buf[4 * 2];

		for (int i = 0; i < 4 * 2; i++) {
			buf[i] = SENTINEL;
		}
		tr_sprite_set_target(buf, 4, 2, test_palette);
		tr_sprite_draw(0, 0, &s);

		unsigned changed = 0;

		for (int i = 0; i < 4 * 2; i++) {
			if (buf[i] != SENTINEL) {
				changed++;
			}
		}
		assert(changed == 6u); /* 8 pixels - 2 transparent */
	}

	/* 4. A sprite clipped at each of the four edges, at both edges of a
	 * corner simultaneously, and a sprite larger than the destination,
	 * writes only the visible part and never outside the buffer -- a guard
	 * region around the actual frame must stay untouched in every case.
	 * Asserts the EXACT visible-cell count each case must produce (not just
	 * "some but not all"), computed by hand below for a 4x4 sprite clipped
	 * into a 6x6 frame. */
	{
#define GW    6u
#define GH    6u
#define GUARD 8u
		static const uint8_t px4[] = {
			/* 4x4, all opaque index 1, high-nibble-first packed. */
			0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
		};
		static const uint8_t px8[] = {
			/* 8x8, all opaque index 1 -- larger than the 6x6 frame below. */
			0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
			0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
			0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x11,
		};
		tr_sprite_t s4 = { .w = 4, .h = 4, .px = px4 };
		tr_sprite_t s8 = { .w = 8, .h = 8, .px = px8 };

		struct {
			uint16_t before[GUARD];
			uint16_t frame[GW * GH];
			uint16_t after[GUARD];
		} storage;

		/* Left, right, top, bottom (single edge each): 4x4 sprite has 2
		 * columns/rows clipped off, 2 remain visible -> 2*4 = 8 cells. */
		const tr_sprite_t *sprites[] = { &s4, &s4, &s4, &s4, &s4, &s8 };
		int                offx[]    = { -2, (int)GW - 2, 2, 2, -2, 0 };
		int                offy[]    = { 2, 2, -2, (int)GH - 2, -2, 0 };
		unsigned           expect[]  = {
			8u,  /* left clip only */
			8u,  /* right clip only */
			8u,  /* top clip only */
			8u,  /* bottom clip only */
			4u,  /* both edges at once (top-left corner): 2 cols * 2 rows */
			36u, /* oversized: an 8x8 sprite fully covers the 6x6 frame */
		};

		for (unsigned c = 0; c < sizeof(sprites) / sizeof(sprites[0]); c++) {
			for (unsigned i = 0; i < GUARD; i++) {
				storage.before[i] = SENTINEL;
				storage.after[i]  = SENTINEL;
			}
			for (unsigned i = 0; i < GW * GH; i++) {
				storage.frame[i] = SENTINEL;
			}
			tr_sprite_set_target(storage.frame, GW, GH, test_palette);
			tr_sprite_draw((int16_t)offx[c], (int16_t)offy[c], sprites[c]);

			for (unsigned i = 0; i < GUARD; i++) {
				assert(storage.before[i] == SENTINEL);
				assert(storage.after[i] == SENTINEL);
			}
			unsigned changed = 0;

			for (unsigned i = 0; i < GW * GH; i++) {
				if (storage.frame[i] != SENTINEL) {
					changed++;
				}
			}
			assert(changed == expect[c]);
		}
#undef GW
#undef GH
#undef GUARD
	}

	/* 5. A sprite drawn entirely off-screen writes nothing. */
	{
		static const uint8_t px[] = { 0x11, 0x11 };
		tr_sprite_t          s    = { .w = 4, .h = 1, .px = px };
		uint16_t             buf[4 * 4];

		for (int i = 0; i < 4 * 4; i++) {
			buf[i] = SENTINEL;
		}
		tr_sprite_set_target(buf, 4, 4, test_palette);
		tr_sprite_draw(1000, 1000, &s);
		tr_sprite_draw(-1000, -1000, &s);
		tr_sprite_draw(-10, 0, &s); /* entirely left of the frame (w=4, x=-10) */
		tr_sprite_draw(0, -10, &s); /* entirely above the frame */

		for (int i = 0; i < 4 * 4; i++) {
			assert(buf[i] == SENTINEL);
		}
	}

	/* 6. A sprite wider than the chunk it's drawn through paints every
	 * chunk -- regression for task-11 fix-round-1 finding 1. render.c's
	 * tr_render_banner() draws a sprite wider than its scratch buffer by
	 * blitting it in chunks, calling tr_sprite_draw() once per chunk with a
	 * NEGATIVE x so chunk N's slice (not the sprite's start) lands at
	 * destination column 0 -- tr_sprite_draw()'s x is a DESTINATION
	 * coordinate, not a source offset. Passing the un-negated chunk offset
	 * silently painted only chunk 0 and left every other chunk blank, with
	 * nothing -- no build error, no other test -- to catch it, because
	 * render.c itself is never host-compiled. An 8x1 sprite drawn through
	 * two 4-wide chunks, mirroring that exact call shape. */
	{
		static const uint8_t px[]    = { 0x11, 0x11, 0x11, 0x11 }; /* 8x1, all opaque index 1. */
		tr_sprite_t          s       = { .w = 8, .h = 1, .px = px };
		const uint16_t       chunk_w = 4;

		for (uint16_t off = 0; off < s.w; off = (uint16_t)(off + chunk_w)) {
			uint16_t buf[4];

			for (int i = 0; i < 4; i++) {
				buf[i] = SENTINEL;
			}
			tr_sprite_set_target(buf, chunk_w, 1, test_palette);
			tr_sprite_draw((int16_t)-off, 0, &s); /* the fix: negative -- a source offset. */

			unsigned painted = 0;

			for (int i = 0; i < 4; i++) {
				if (buf[i] != SENTINEL) {
					painted++;
				}
			}
			assert(painted == 4u); /* every chunk fully painted, not just chunk 0 */
		}
	}

	/* 7. tr_score_to_digits(): the score HUD's decimal decomposition (F7,
	 * whole-branch review). Pure logic, kept in sprite.c specifically so it
	 * is host-testable -- render.c, where it is consumed, is not. */
	{
		uint8_t d[TR_SCORE_MAX_DIGITS];
		int     n;

		/* Zero is one digit, "0" -- not zero digits. */
		n = tr_score_to_digits(0u, d);
		assert(n == 1 && d[0] == 0);

		/* A single-digit score. */
		n = tr_score_to_digits(7u, d);
		assert(n == 1 && d[0] == 7);

		/* A multi-digit score decomposes most-significant-first. */
		n = tr_score_to_digits(1234u, d);
		assert(n == 4 && d[0] == 1 && d[1] == 2 && d[2] == 3 && d[3] == 4);

		/* The largest supported score: TR_SCORE_MAX_DIGITS 9s, exactly. */
		uint32_t max_val = 1;

		for (int i = 0; i < TR_SCORE_MAX_DIGITS; i++) {
			max_val *= 10u;
		}
		max_val -= 1u;
		n = tr_score_to_digits(max_val, d);
		assert(n == TR_SCORE_MAX_DIGITS);
		for (int i = 0; i < TR_SCORE_MAX_DIGITS; i++) {
			assert(d[i] == 9);
		}

		/* Past the largest supported score: saturates to the same all-9s
		 * display, an honest cap, rather than silently truncating to a
		 * materially wrong, smaller-looking number. */
		n = tr_score_to_digits(max_val + 1u, d);
		assert(n == TR_SCORE_MAX_DIGITS);
		for (int i = 0; i < TR_SCORE_MAX_DIGITS; i++) {
			assert(d[i] == 9);
		}
		n = tr_score_to_digits(0xFFFFFFFFu, d); /* far past it, not just by one */
		assert(n == TR_SCORE_MAX_DIGITS);
		for (int i = 0; i < TR_SCORE_MAX_DIGITS; i++) {
			assert(d[i] == 9);
		}
	}

	return 0;
}
