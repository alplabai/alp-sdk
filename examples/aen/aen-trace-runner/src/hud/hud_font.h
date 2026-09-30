/* src/hud/hud_font.h -- the types tools/genhud.py emits into hud_assets.h. */
#ifndef TR_HUD_FONT_H
#define TR_HUD_FONT_H

#include <stdint.h>

/* One glyph: a w x h 4-bit alpha map placed at pen + (x, y), y measured down
 * from the font's top line (ascent above the baseline); adv moves the pen. */
typedef struct {
	uint8_t  w, h;
	int8_t   x, y;
	uint8_t  adv;
	uint32_t off; /* into the font's bits[] */
} tr_glyph_t;

typedef struct {
	uint8_t           asc;  /* top line to baseline, px */
	uint8_t           line; /* line height, px */
	const tr_glyph_t *g;    /* [96]: chars 32..127, 127 = U+00B7 */
	const uint8_t    *bits;
} tr_font_t;

#endif /* TR_HUD_FONT_H */
