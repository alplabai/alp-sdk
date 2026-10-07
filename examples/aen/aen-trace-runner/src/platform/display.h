/* src/platform/display.h */
#ifndef TR_PLATFORM_DISPLAY_H
#define TR_PLATFORM_DISPLAY_H

#include <stdint.h>

/* 0 on success; -1 for the intermittent panel-init defect (retry-worthy);
 * -2 for a deterministic configuration failure (not retry-worthy). See the
 * doc comment on this function in display.c. */
int      tr_display_open(void);
uint16_t tr_display_width(void);
uint16_t tr_display_height(void);
/* Clockwise degrees (0, 90, 270) the frame is turned onto the panel: the
 * display's mount-rotation (alp_display_caps_t.rotation). The A32 renderer
 * and the HUD apply it (render/panel_rot.h); the M55 2D renderer cannot, and
 * display.c refuses a panel that needs it at build time. */
uint16_t tr_display_rotation(void);

#if TR_RENDER_A32
/*
 * TR_RENDER=A32 (display_a32.c): the M55 writes no pixels. The A32 draws each
 * frame into tr_display_free_fb(); tr_display_flip_to() swaps the CDC200 to it
 * in vertical blanking and blocks until it has landed (~25.0 ms at 40.0 Hz,
 * ~33.3 ms at 30.0 Hz on a 30 Hz panel).
 * Returns 0 on a landed flip, -1 if `fb_addr` is not TR_FB_A/TR_FB_B, is
 * already live, or the swap never landed (the old frame stays up).
 */
uint32_t tr_display_free_fb(void); /* TR_FB_A or TR_FB_B; 0 if the live buffer is neither */
int      tr_display_flip_to(uint32_t fb_addr);
/* After a deliberate hold (game-over, fallback banner): the next flip's gap is
 * not a missed refresh, so it does not count toward tr_frame_overrun_count. */
void tr_display_pace_reset(void);
#else

/*
 * Blit into the BACK buffer -- never into the buffer the CDC200 is scanning
 * out. Nothing reaches the glass until tr_display_flip().
 */
int tr_display_blit(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const void *px);

/*
 * Show everything blitted since the last flip, and block until the swap has
 * actually landed (the CDC200 applies it in vertical blanking -- see
 * display.c). Call it ONCE per picture, after all of that picture's drawing:
 * a picture that is never flipped is never seen, and two flips in one tick
 * cost two vertical-blanking waits (~25.0 ms each at the panel's 40.0 Hz).
 * A flip with nothing drawn since the last one is free.
 */
void tr_display_flip(void);

/*
 * NOT part of the double-buffered path: this goes through the SDK's own
 * clear op, which writes whichever buffer the CDC200 driver currently
 * scans out -- i.e. straight onto the glass, mid-frame. Unused; callers
 * want tr_render_init() (render.h), which paints the back buffer.
 */
int tr_display_clear(void);
#endif /* TR_RENDER_A32 */

#endif /* TR_PLATFORM_DISPLAY_H */
