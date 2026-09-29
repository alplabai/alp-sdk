/* src/render/render.h */
#ifndef TR_RENDER_H
#define TR_RENDER_H

#include "../game/state.h"
#include "sprite.h"

/*
 * The runner sprite is 96x96 and the obstacles are 88x88 (see
 * tools/genart.py / src/render/atlas.h) -- both must fit this scratch
 * buffer, or the BUILD_ASSERTs in render.c catch it at build time, not at
 * link time. g_buf grows with this: 96*96*2 = 18,432 B (was 64*64*2 =
 * 8,192 B; see task-11-brief.md step 4b).
 */
#define TR_SPRITE_MAX_W 96
#define TR_SPRITE_MAX_H 96

/*
 * (Re)initialise the renderer for a w x h panel: resets the erase
 * bookkeeping AND paints the whole panel to COLOR_BG (render.c) -- callers
 * do not also need tr_display_clear() (platform/display.h); calling both
 * would just paint the panel twice with different colours, which is the
 * bug this replaced (whole-branch review F3).
 */
void tr_render_init(uint16_t w, uint16_t h);
void tr_render_frame(const tr_game_t *g);

/*
 * Draw banner sprite `s` (a fixed prompt image -- "step into view",
 * "step back into view", "check the camera", "game over") across the top of
 * the panel, on a plate this function paints procedurally (see render.c;
 * `s` is text-only). `s` is wider than this file's scratch buffer
 * (TR_SPRITE_MAX_W), so it is blitted in TR_SPRITE_MAX_W-wide chunks,
 * sampling a different horizontal slice of the sprite each time; see the
 * comment in render.c. Goes through paint(), the sole bounds authority
 * here, instead of a second, unchecked blit path.
 */
void tr_render_banner(const tr_sprite_t *s);

/*
 * Accessors for the fixed prompt banners src/render/atlas.h defines. Callers
 * outside render.c (main.c) get a pointer through these instead of including
 * atlas.h themselves -- atlas.h's sprite data is `static`, included in this
 * one translation unit only, so a second include would duplicate the whole
 * atlas (tens of KB) rather than share it.
 */
const tr_sprite_t *tr_banner_stand(void);
const tr_sprite_t *tr_banner_step_back(void);
const tr_sprite_t *tr_banner_check_camera(void);
const tr_sprite_t *tr_banner_game_over(void); /* F7, whole-branch review: the death state. */
const tr_sprite_t *tr_banner_attract(void);   /* The self-play invitation -- see game/attract.h. */

#endif /* TR_RENDER_H */
