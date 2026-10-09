/* src/ipc/tr_vol.h -- the game sound's volume: one shared record in the SRAM0 mailbox page, a
 * software gain on the HP's mixed samples, and the HE's rules for who may change it.
 *
 * WHY SOFTWARE. The HP's TAS2563 amps are programmed once over I2C2, and I2C2 is leased from
 * the HE (src/ipc/tr_bus2.h): a volume change must never need the bus. So the HP scales every
 * 16 ms block of mixed S16 samples (tr_vol_apply) and the stream never stops. 0 is silence, not
 * a shutdown: the amps keep running, SD_N (GPIO5 P5_2) is never toggled. TR_SND_VOLUME
 * (sound/src/main.c) stays the hardware ceiling the amps are set to, bench-capped at 128: 100 %
 * here is that level, so the word can only turn the sound DOWN from what was heard to be safe.
 *
 * WHERE. TR_MEM_VOL (tr_memmap.h) 0x0237FD80, 16 B, in the shared NC page. Every word has ONE
 * writer:
 *   vol      HE   what the HP applies:  TR_VOL_TAG | percent
 *   req      the bench, over SWD: "please set":  TR_VOL_TAG | percent  (the HE validates it);
 *            0 = no request (writing it re-arms the same value, it is not a refusal)
 *   rejects  HE   requests it refused (bad tag or percent above 100)
 *   seq      HE   changes adopted, for the HUD's "VOL" popup
 * A cold SRAM0 holds garbage: a vol word without the tag (or with a percent above 100) is read
 * as TR_VOL_DEFAULT, never as a level. The HE rewrites vol at boot and again whenever it does
 * not hold the HE's own level, so writing vol over SWD does nothing -- write req.
 *
 * SOURCES the HE adopts, in one place (tr_vol_he_step): the EVK rotary encoder (TR_VOL_STEP per
 * detent), its push switch (mute / unmute) and the bench's req word.
 *
 * Pure C, no Zephyr: tests/host/test_vol.c runs exactly what both cores run.
 */
#ifndef TR_VOL_H
#define TR_VOL_H

#include <stdbool.h>
#include <stdint.h>

#include "tr_bus2.h"
#include "tr_memmap.h"

#define TR_VOL_TAG      0x564F0000u /* 'VO' */
#define TR_VOL_TAG_MASK 0xFFFF0000u
#define TR_VOL_MAX      100u /* percent of TR_SND_VOLUME */
#define TR_VOL_DEFAULT  100u /* an unset or garbage word */
#define TR_VOL_STEP     5u   /* percent per encoder detent */

typedef struct {
	uint32_t vol;
	uint32_t req;
	uint32_t rejects;
	uint32_t seq;
} tr_vol_t;

_Static_assert(sizeof(tr_vol_t) == 16u, "tr_vol_t is 4 words on the wire");
_Static_assert(TR_MEM_VOL % 16u == 0u, "the record is 16-B aligned");
_Static_assert(TR_MEM_VOL >= TR_MEM_BUS2 + sizeof(tr_bus2_t),
               "the volume record starts after the I2C2 lease record");
_Static_assert(TR_MEM_VOL + sizeof(tr_vol_t) <= TR_MHU0_WINDOW_LO,
               "the volume record sits inside the shared NC page below the MHU0 window");
_Static_assert(TR_MEM_VOL >= TR_MEM_ARING &&
                   TR_MEM_VOL + sizeof(tr_vol_t) <= TR_MEM_ARING + 0x1000u,
               "the volume record is inside the shared NC page (0x0237F000..0x0237FFFF)");

static inline uint32_t tr_vol_word(uint32_t pct)
{
	return TR_VOL_TAG | (pct > TR_VOL_MAX ? TR_VOL_MAX : pct);
}

/* A tagged word with a percent in range. *pct is written only when it is. */
bool tr_vol_valid(uint32_t word, uint32_t *pct);

/* The level a vol word means: its percent, or TR_VOL_DEFAULT for anything else. */
uint32_t tr_vol_read(uint32_t word);

/* ---- HP: the gain ----------------------------------------------------------------------- */
#define TR_VOL_UNITY 65536u /* Q16: 100 % */

typedef struct {
	uint32_t gain; /* Q16, where the last block ended */
} tr_vol_ramp_t;

uint32_t tr_vol_gain(uint32_t pct); /* Q16 linear amplitude: pct / 100 */

/* Start at `pct` (no ramp from unity on the first block). */
void tr_vol_ramp_init(tr_vol_ramp_t *r, uint32_t pct);

/* Scale buf[0..n) in place toward `pct`: the gain moves linearly from where the last block ended
 * to the target across this block, so a step of any size is a 16 ms ramp, never a click. A block
 * that starts and ends at unity is left bit-exact (the common case costs nothing). */
void tr_vol_apply(tr_vol_ramp_t *r, int16_t *buf, unsigned n, uint32_t pct);

/* ---- HE: the owner ---------------------------------------------------------------------- */
typedef struct {
	uint32_t pct;
	uint32_t unmute_pct; /* what the switch restores: the last non-zero level */
	uint32_t last_req;   /* the req word as last seen: only a CHANGE is a request */
} tr_vol_he_t;

/* HE boot: level = TR_VOL_DEFAULT, publish it, zero rejects and seq (SRAM0 powers up with
 * garbage and survives warm resets). A req left over from before is remembered, not obeyed. */
void tr_vol_he_boot(tr_vol_he_t *he, volatile tr_vol_t *r);

/* One HE frame. `detents`: encoder detents since the last call (clockwise positive); `press`:
 * the switch went down. Order: encoder, switch, then the bench's req. Re-asserts the vol word if
 * anything else changed it. Returns true when the level changed. */
bool tr_vol_he_step(tr_vol_he_t *he, volatile tr_vol_t *r, int32_t detents, bool press);

#endif /* TR_VOL_H */
