/* src/ipc/tr_knob.h -- the EVK encoder's two jobs: its push switch picks what the knob turns
 * (VOLUME or BRIGHTNESS), and the panel backlight's level with the bench's request word.
 *
 * THE SWITCH. Short press (released before TR_KNOB_LONG_MS): toggle the mode VOLUME <->
 * BRIGHTNESS. Long press (held TR_KNOB_LONG_MS): mute / unmute the game sound (tr_vol.h: the level
 * before the mute is restored, a turn while muted unmutes). The long press fires WHILE held, so
 * the mute is heard at once; the release after it does nothing. A short press while muted still
 * toggles the mode (only a long press unmutes). BRIGHTNESS reverts to VOLUME after
 * TR_KNOB_IDLE_MS with no turn and no press. The raw switch must hold a new state
 * TR_KNOB_DEBOUNCE_MS before it counts, so a contact bounce is neither a press nor a short one.
 *
 * THE BACKLIGHT. TR_BL_MIN..TR_BL_MAX percent in TR_BL_STEP steps; the floor is hard, so neither
 * the knob nor the bench word can turn the panel dark. The HE owns the PWM (UTIMER3 on P10_7,
 * through the LED API), so nothing crosses a core but the bench record TR_MEM_BL:
 *   bl       HE      the level now:  TR_BL_TAG | percent
 *   req      bench   "please set":   TR_BL_TAG | percent, TR_BL_MIN..TR_BL_MAX (0 = no request)
 *   rejects  HE      requests refused (bad tag, or a percent outside the range)
 *   seq      HE      level changes adopted
 * The boot level is the one the SDK already set (alp,display-backlight default-brightness), so
 * booting never flickers the panel.
 *
 * Pure C, no Zephyr: tests/host/test_knob.c runs what the HE runs.
 */
#ifndef TR_KNOB_H
#define TR_KNOB_H

#include <stdbool.h>
#include <stdint.h>

#include "tr_memmap.h"
#include "tr_vol.h"

#define TR_BL_TAG  0x424C0000u /* 'BL' */
#define TR_BL_MIN  10u         /* percent: never dark */
#define TR_BL_MAX  100u
#define TR_BL_STEP 5u /* percent per encoder detent */

#define TR_KNOB_DEBOUNCE_MS 30u
#define TR_KNOB_LONG_MS     1000u
#define TR_KNOB_IDLE_MS     5000u

typedef struct {
	uint32_t bl;
	uint32_t req;
	uint32_t rejects;
	uint32_t seq;
} tr_bl_t;

_Static_assert(sizeof(tr_bl_t) == 16u, "tr_bl_t is 4 words on the wire");
_Static_assert(TR_MEM_BL % 16u == 0u, "the record is 16-B aligned");
_Static_assert(TR_MEM_BL >= TR_MEM_VOL + sizeof(tr_vol_t),
               "the backlight record starts after the volume record");
_Static_assert(
    TR_MEM_BL >= TR_MEM_BL_GAP_LO && TR_MEM_BL + sizeof(tr_bl_t) <= TR_MEM_BL_GAP_HI,
    "the backlight record sits in the gap between the fix/sn65dsi83-auto-recovery words");
_Static_assert(TR_MEM_BL + sizeof(tr_bl_t) <= TR_MHU0_WINDOW_LO,
               "the backlight record sits inside the shared NC page below the MHU0 window");

static inline uint32_t tr_bl_word(uint32_t pct)
{
	return TR_BL_TAG | (pct > TR_BL_MAX ? TR_BL_MAX : pct);
}

/* A tagged word with a percent in TR_BL_MIN..TR_BL_MAX. *pct is written only when it is. */
bool tr_bl_valid(uint32_t word, uint32_t *pct);

typedef enum { TR_KNOB_VOLUME = 0, TR_KNOB_BRIGHTNESS = 1 } tr_knob_mode_t;

typedef struct {
	tr_knob_mode_t mode;
	uint32_t       bl_pct;      /* the backlight level now */
	uint32_t       seq;         /* mode / level / mute events: the HUD popup's trigger */
	uint32_t       last_req;    /* the req word as last seen */
	uint32_t       last_act_ms; /* last turn / press / bench request */
	uint32_t       raw_ms;      /* when the raw switch last changed */
	uint32_t       down_ms;     /* when the debounced press began */
	bool           has_bl;      /* a backlight exists: without one a short press does nothing */
	bool           raw;
	bool           down;       /* debounced */
	bool           long_fired; /* this press already muted */
} tr_knob_t;

typedef struct {
	int32_t vol_detents; /* feed to tr_vol_he_step */
	bool    mute;        /* long press: feed as tr_vol_he_step's press */
	bool    bl_changed;  /* apply k->bl_pct to the LED */
} tr_knob_out_t;

/* HE boot: the backlight is already at `bl_pct` (published, not re-applied). A req left over
 * from before is remembered, not obeyed. */
void tr_knob_boot(tr_knob_t *k, volatile tr_bl_t *r, uint32_t bl_pct, bool has_bl);

/* One HE frame at `now_ms`: `detents` since the last call (clockwise positive), `sw` the raw
 * switch (true = pressed). Order: switch, encoder, then the bench's req. */
tr_knob_out_t
tr_knob_step(tr_knob_t *k, volatile tr_bl_t *r, uint32_t now_ms, int32_t detents, bool sw);

#endif /* TR_KNOB_H */
