/* src/platform/volume_he.h -- the HE's side of the game sound's volume (TR_HP_SOUND builds; the
 * record, the rules and the HP's gain: src/ipc/tr_vol.h). Without TR_HP_SOUND there is no HP
 * sound to turn down and every call compiles to nothing (seq stays 0: the HUD shows no popup). */
#ifndef TR_PLATFORM_VOLUME_HE_H
#define TR_PLATFORM_VOLUME_HE_H

#include <stdint.h>

#ifndef TR_HP_SOUND
#define TR_HP_SOUND 0
#endif

#if TR_HP_SOUND
/* Once, at boot: publish the default level, open the EVK's rotary encoder (EVK_ENC_ROTARY, its
 * detents set the volume or, after a short press, the backlight) and push switch
 * (EVK_PIN_ENCODER_SW: short = volume / brightness, long = mute / unmute; tr_knob.h). Either one failing
 * to open leaves the other and the bench's request word working. */
void tr_volume_he_init(void);

/* Once per game frame (main loop, never an ISR): read the encoder and the switch, adopt the
 * bench's request word, publish the level. Never touches I2C2 or GPIO5. */
void tr_volume_he_frame(void);

/* What the HUD's popup shows: the quantity the knob is set to (TR_HUD_KNOB_*, hud.h) and its level
 * in percent (the volume, 0 = mute; or the brightness), and a count that moves on every change of
 * either, of the mode and of the mute: the popup's trigger. */
uint8_t  tr_volume_he_pct(void);
uint8_t  tr_volume_he_kind(void);
uint32_t tr_volume_he_seq(void);
#else
static inline void tr_volume_he_init(void)
{
}

static inline void tr_volume_he_frame(void)
{
}

static inline uint8_t tr_volume_he_pct(void)
{
	return 100u;
}

static inline uint8_t tr_volume_he_kind(void)
{
	return 0u;
}

static inline uint32_t tr_volume_he_seq(void)
{
	return 0u;
}
#endif

#endif /* TR_PLATFORM_VOLUME_HE_H */
