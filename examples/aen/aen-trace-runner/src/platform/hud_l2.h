/* src/platform/hud_l2.h -- the HUD on CDC200 layer 2 (TR_RENDER=A32). */
#ifndef TR_PLATFORM_HUD_L2_H
#define TR_PLATFORM_HUD_L2_H

#include <stdbool.h>
#include <stdint.h>

#include "../game/hiscore.h"
#include "../game/score.h"
#include "../game/zone.h"

/* After tr_display_open(): clear the HUD buffer and bring layer 2 up (see
 * hud_l2.c). false: the CDC200 has no layer 2 -- the A32 keeps drawing its
 * own HUD (frames are not flagged TR_FLAG_HUD_L2). */
bool tr_hud_l2_open(void);

/* Layer 2 is up: flag every frame TR_FLAG_HUD_L2. */
bool tr_hud_l2_up(void);

/* Once per presented frame, right after it is published to the A32 (so the
 * repaint overlaps the A32's render, never the flip wait): the run's points,
 * the frame's banner (TR_BANNER_*), attract, the invitation
 * (TR_HUD_INVITE_*), the world zone (its name on entry, P15) and the
 * character (TR_CHAR_*, P16). Repaints only what changed. */
void tr_hud_l2_present(const tr_score_t    *s,
                       uint8_t              banner,
                       bool                 attract,
                       uint8_t              invite,
                       const tr_zone_t     *z,
                       uint8_t              character,
                       const tr_hiscore_t  *hs,
                       const tr_initials_t *ini);

/* Counters the perf panel reads (display_a32.c, a32.c). */
extern volatile uint32_t tr_flip_count;
extern volatile uint64_t tr_a32_busy_ticks[2];
#if TR_M55_AUTOLAUNCH
extern const volatile uint32_t tr_a32_autolaunch_id[3];
#endif
bool tr_a32_link_ok(void);

#endif /* TR_PLATFORM_HUD_L2_H */
