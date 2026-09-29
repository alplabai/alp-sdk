/* src/ipc/tr_aring.h -- game-sound event ring, M55-HE (producer) -> M55-HP
 * (consumer), in shared SRAM0.
 *
 * The HE runs the game and pushes one 4-byte event per sound-worthy moment
 * (src/game/sfx.h decides which); the HP pops them and feeds the synth
 * (src/audio/tr_audio.h), which renders the I2S stream (sound/). Single
 * producer, single consumer, free-running 32-bit indices: head is written
 * only by the HE, tail only by the HP, so no lock and no atomic RMW across
 * masters is needed -- only ordered stores (the caller's barrier, as in
 * tr_mbox.h).
 *
 * Address: SRAM0 0x0237F000, the top 4 KiB of the "spare" 0x02335000..
 * 0x0237FFFF in docs/superpowers/plans/2026-09-22-a32-renderer.md section 4,
 * directly below the TF-A MHU0 window (0x02380000). Clear of FB A
 * (0x02000000..0x021C1FFF), the renderer scratch/DL (0x02200000..
 * 0x02334FFF), the MHU0 window, SRAM1 (mailbox, stub, renderer, FB B) --
 * asserted below. SRAM0 is powered from reset, unlike SRAM1, so the HP can
 * poll it before the A32 chain is up. Both M55 builds run CONFIG_DCACHE=n,
 * so no cache maintenance is needed.
 *
 * NOT for TR_RENDER=M55: that build's framebuffer is the shield's lcd_fb
 * (0x02200000..0x023C1FFF), which covers this address. main.c hooks the ring
 * up in the A32 build only.
 */
#ifndef TR_ARING_H
#define TR_ARING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "tr_memmap.h" /* TR_MEM_ARING: where the ring lives, with every other fixed region */

#define TR_ARING_ADDR    TR_MEM_ARING /* tr_memmap.h: SRAM0 0x0237F000 */
#define TR_ARING_MAGIC   0x54524155u /* 'TRAU' */
#define TR_ARING_VERSION 1u
#define TR_ARING_CAP     64u /* power of two: index & (CAP - 1) */

/* Event kinds (tr_aev_t.kind). `param` meaning per kind in brackets. */
#define TR_AEV_NONE      0u
#define TR_AEV_FOOTSTEP  1u /* [0/1 which foot] */
#define TR_AEV_PICKUP    2u /* [combo: 0 = first pickup, n = n-th in a streak; pitch rises] */
#define TR_AEV_JUMP      3u
#define TR_AEV_DUCK      4u
#define TR_AEV_CRASH     5u /* [tr_mbox.h TR_CRASH_KIND_*: 1 low, 2 high, 3 live wire] */
#define TR_AEV_WIRE      6u /* [intensity 0..255: live-wire arc hum/crackle, P4b re-sends while near] */
#define TR_AEV_ATTRACT   7u /* attract jingle */
#define TR_AEV_GAME_OVER 8u /* game-over sting */
#define TR_AEV_MUSIC     9u /* [TR_MUSIC_*] */
#define TR_AEV_COUNT     10u

#define TR_MUSIC_OFF     0u
#define TR_MUSIC_ATTRACT 1u /* music at attract level */
#define TR_MUSIC_PLAY    2u /* music at play level */

typedef struct {
	uint8_t kind, param, pad0, pad1;
} tr_aev_t;
_Static_assert(sizeof(tr_aev_t) == 4, "tr_aev_t is 4 B on the wire");

/* tr_aring_t.hp_state */
#define TR_ARING_HP_OFF     0u
#define TR_ARING_HP_RUNNING 1u /* HP audio running, I2S streaming */
#define TR_ARING_HP_FAULT   2u /* HP up but no audio: bring-up failed, or an I2S write storm (hp_fault_step) */

typedef struct {
	/* +0x00 identity, HE writes (magic last) */
	uint32_t magic, version, pad0[14];
	/* +0x40 HE -> HP */
	uint32_t head;    /* next slot the HE writes; bumped AFTER the slot + barrier */
	uint32_t dropped; /* pushes refused because the ring was full */
	uint32_t pad1[14];
	/* +0x80 HP -> HE / bench (read over SWD) */
	uint32_t tail;          /* next slot the HP reads */
	uint32_t hp_state;      /* TR_ARING_HP_* */
	uint32_t hp_heartbeat;  /* +1 per rendered block */
	uint32_t hp_underruns;  /* I2S writes that failed/timed out */
	uint32_t hp_fault_step; /* bring-up step that failed, or 12 = streaming underrun (sound/src/main.c) */
	uint32_t hp_events;     /* events consumed */
	uint32_t pad2[10];
	/* +0xC0 */
	tr_aev_t ev[TR_ARING_CAP];
} tr_aring_t;
_Static_assert(sizeof(tr_aring_t) == 0x1C0, "tr_aring_t layout drifted");
_Static_assert(offsetof(tr_aring_t, head) == 0x40, "HE block moved");
_Static_assert(offsetof(tr_aring_t, tail) == 0x80, "HP block moved");
_Static_assert(offsetof(tr_aring_t, ev) == 0xC0, "event array moved");
_Static_assert(sizeof(tr_aring_t) == TR_MEM_ARING_SIZE, "tr_memmap.h TR_MEM_ARING_SIZE != sizeof(tr_aring_t)");
_Static_assert((TR_ARING_CAP & (TR_ARING_CAP - 1u)) == 0u, "TR_ARING_CAP must be a power of two");

/* The ring must stay inside the documented spare and out of every other
 * shared region (see the file header). */
_Static_assert(TR_ARING_ADDR >= 0x02335000u && TR_ARING_ADDR + sizeof(tr_aring_t) <= 0x02380000u,
               "tr_aring_t must sit inside SRAM0 spare 0x02335000..0x0237FFFF (below the MHU0 window)");

/* HE: make the ring usable. A ring already carrying the magic (HP running,
 * HE rebooted) keeps its indices and just drops unread events (head = tail);
 * anything else is zeroed and the magic written last. Either way hp_state
 * goes back to TR_ARING_HP_OFF: SRAM0 survives a warm reset, a live HP
 * rewrites it every 16 ms, and without one the HUD must read "--", not a
 * previous session's "audio". */
void tr_aring_init(volatile tr_aring_t *r, void (*barrier)(void));

/* HE: queue one event. false (and dropped++) if the ring is full. */
bool tr_aring_push(volatile tr_aring_t *r, uint8_t kind, uint8_t param, void (*barrier)(void));

/* HP: take the oldest event. false if none, or if the ring is not
 * initialised yet or carries another TR_ARING_VERSION. A head more than CAP ahead of tail (corrupt, or a
 * producer that restarted from zero) resyncs tail to head and returns false. */
bool tr_aring_pop(volatile tr_aring_t *r, tr_aev_t *out, void (*barrier)(void));

#endif /* TR_ARING_H */
