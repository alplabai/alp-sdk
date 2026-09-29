/* src/ipc/tr_mbox.h -- SRAM1 shared-memory mailbox, M55 (HE) <-> the A32 pair.
 *
 * Compiled into four places: the M55 image, the A32 renderer, the A32 stub,
 * and the host tests -- see docs/superpowers/plans/2026-09-22-a32-renderer.md
 * section 6. Fixed-width integers ONLY in every wire struct below: no
 * `enum`, no `bool`. `tr_game_t` (src/game/state.h) has both, so it is never
 * embedded here -- tr_frame_in_from_game() copies it field by field.
 *
 * NOTE ON THIS HEADER vs THE PLAN DOC: section 6's tr_frame_in_t is
 * commented "144 B", and its tr_mbox_t lays the `in` payload block at
 * +0x080..+0x100 (128 B) with a total size of 0x1C0. Compiled, the struct as
 * literally listed (16 tr_pkt_ent_t entries at 8 B each, plus its 24 B
 * header) is 152 B -- neither figure holds, and 152 B does not fit the
 * claimed 128 B slot at all (confirmed with a standalone probe, matching
 * this file's _Static_assert below). Trimming to 13 entries would fit the
 * slot but silently drops up to 3 of TR_MAX_ENTITIES's 16 live entities from
 * what the A32 renders -- a real gameplay bug, not a layout nit. Kept all 16
 * entities (matches state.h's TR_MAX_ENTITIES, no data loss) and widened the
 * `in` block by one 64 B block instead: tr_frame_in_t is 152 B, the `in`
 * block is 0x080..0x140 (192 B, still 64 B aligned), and every block after
 * it shifts by 0x040. Real totals: sizeof(tr_frame_in_t) == 152 (160 since P6, 168 since P16, 172 since P15, see tr_frame_in_t),
 * sizeof(tr_mbox_t) == 0x200, blocks at +0x040 +0x080 +0x140 +0x180 +0x1C0.
 * Flag for the maintainer if the plan doc's numbers were meant to imply a
 * smaller entity count instead.
 */
#ifndef TR_MBOX_H
#define TR_MBOX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../game/react.h" /* tr_react_t -- source of the P16 react_* fields */
#include "../game/zone.h"  /* tr_zone_t -- tr_frame_in_set_zone() */
#include "../game/state.h" /* tr_game_t -- source struct for tr_frame_in_from_game(), never embedded on the wire */
#include "../game/tilt.h" /* TR_TILT_CHARS */

#define TR_MBOX_ADDR    0x02401000u
#define TR_MBOX_MAGIC   0x54524D42u /* 'TRMB' */
#define TR_MBOX_VERSION 1u
#define TR_FB_A         0x02000000u /* SRAM0, DT sram0 */
/* FB B: SRAM1 0x02600000..0x027C1FFF, below TF-A RW 0x027DE000 (the
 * CDC200 scans SRAM1 fine, measured). It used to be the shield's lcd_fb
 * 0x02200000, which overlaps the TF-A MHU0 payload window. Override only to
 * pair with an old test payload: -DTR_FB_B_ADDR=0x02200000u. */
#ifndef TR_FB_B_ADDR
#define TR_FB_B_ADDR 0x02600000u
#endif
#define TR_FB_B    TR_FB_B_ADDR
#define TR_FB_SIZE 1843200u /* 720 x 1280 RGB565 */
/* TF-A MHU0 payload window (== a32/common/stub_abi.h STUB_MHU0_WINDOW):
 * no A32 code ever writes it, so no framebuffer may contain it. */
#define TR_MHU0_WINDOW_LO 0x02380000u
#define TR_MHU0_WINDOW_HI 0x02381000u
#define TR_FB_CLEAR_OF_MHU0(fb) \
	((fb) + TR_FB_SIZE <= TR_MHU0_WINDOW_LO || (fb) >= TR_MHU0_WINDOW_HI)
/* HUD buffer (P9): CDC200 layer 2, drawn by the HE only (src/hud/hud.h:
 * 720 x 352 ARGB4444). SRAM0 above the MHU0 window, below the stub's early
 * fault park page (a32/common/stub_abi.h STUB_EARLY_PARK 0x023FE000): no
 * A32 table maps it (the renderer's 0x023 pages fault from 0x02380000). */
#define TR_HUD_FB      0x02382000u
#define TR_HUD_FB_SIZE 506880u /* 720 x 352 x 2 */
/* Renderer bench block (a32/renderer/renderer.c RENDER_STATS) word 6: the
 * renderer's image + .bss end address, written at LAUNCH (0 from an older
 * renderer). The HE perf panel reads it for the SRAM figure. */
#define TR_RENDER_IMG_END_ADDR 0x02401918u
/* A32 LAUNCH timing stamps, CNTVCT low 32 bits (100 MHz; subtract, never
 * compare), for the HE's first-frame log line (src/platform/a32.c) and
 * decode.py. Stub, in tr_mbox_t.pad4[] (== stub_abi.h MBOX_OFF_T_*): */
#define TR_STUB_T_COPY0  4u /* release MRAM -> SRAM copy starts (0: dev boot, no copy) */
#define TR_STUB_T_COPY1  5u /* copy + D-cache clean done */
#define TR_STUB_T_LAUNCH 6u /* launch() entry (every LAUNCH): CRC of the image next */
#define TR_STUB_T_JUMP   7u /* CRC + cache sync done, jumping to the payload */
/* Renderer, bench block words 12..14 (a32/renderer/renderer.c RENDER_STATS;
 * words 7..11 belong to feat/edge-aa and feat/dma-copyout): renderer_main
 * entry, self-checks + render_init done, first out_seq published (0 until
 * then) -- all rewritten at every LAUNCH. Words 15..16 (fix round 8 item 3):
 * the bottom video panel's own cost, us -- this frame and the max since
 * LAUNCH. Published here (not render_core_stats, which stays in D-cache and
 * reads 0 to a raw external SRAM read unless explicitly cleaned) because
 * this block is the one already proven bench-readable without a prof build
 * (decode.py's own use of words 12..14). */
#define TR_RENDER_T_ADDR                  0x02401930u
#define TR_RENDER_STATS_ADDR              0x02401900u
#define TR_RENDER_STATS_MARKER            0x5E4D5354u
#define TR_RENDER_VIDEO_PANEL_US_ADDR     (TR_RENDER_STATS_ADDR + 15u * 4u) /* this frame */
#define TR_RENDER_VIDEO_PANEL_US_MAX_ADDR (TR_RENDER_STATS_ADDR + 16u * 4u) /* max since LAUNCH */

/* tr_mbox_t.ctrl_cmd and tr_mbox_t.stub_state values. */
#define TR_CTRL_NONE    0u
#define TR_CTRL_LAUNCH  1u
#define TR_CTRL_HALT    2u
#define TR_STUB_PARKED  0u
#define TR_STUB_RUNNING 1u
#define TR_STUB_FAULT   2u

/* tr_frame_in_t.flags bits. */
#define TR_FLAG_ALIVE          (1u << 0)
#define TR_FLAG_AIRBORNE       (1u << 1)
#define TR_FLAG_DUCKING        (1u << 2)
#define TR_FLAG_ATTRACT_ACTIVE (1u << 3)
#define TR_FLAG_PAUSED         (1u << 4)
#define TR_FLAG_CRASH          (1u << 5) /* crash sequence frame: crash_* below are valid */
#define TR_FLAG_PHASE          (1u << 6) /* `phase` below is valid (an old HE never wrote it) */
#define TR_FLAG_HUD_L2 \
	(1u << 7) /* the HE draws score + banners on CDC200 layer 2: the A32 draws none */
#define TR_FLAG_CHAR \
	(1u << 8) /* character / react_* below are valid (P16; an old HE never wrote them) */
#define TR_FLAG_IDLE (1u << 9) /* the runner stands idle (attract lobby, no run): idle_ms below */

/* tr_frame_in_t.character values (P16): meshes.h tr_rig_chars[] order. */
#define TR_CHAR_PROBE  0u
#define TR_CHAR_SOLDER 1u
#define TR_CHAR_FLUX   2u
#define TR_CHAR_PIXEL  3u
#define TR_CHAR_N      4u
_Static_assert(TR_CHAR_N == TR_TILT_CHARS, "game/tilt.h picks from TR_TILT_CHARS characters");

/* tr_frame_in_t.react values (P16): the latest reaction-worthy event of the
 * run (src/game/react.h), which the renderer animates for react_ms. */
#define TR_REACT_NONE   0u
#define TR_REACT_PASS   1u /* an obstacle went by (cleared, or in another lane): glance back */
#define TR_REACT_PICKUP 2u /* fist pump */
#define TR_REACT_NEAR   3u /* cleared by a small margin: stumble, squint */
#define TR_REACT_COMBO  4u /* a combo milestone (x3, x5): spin, happy eyes */
#define TR_FLAG_ZONE \
	(1u << 10) /* `zone` + `gate_y` below are valid (P15; an old HE: zone 0, no gate) */
/* Bit 11 is TR_FLAG_DMA0_CLK on a parked branch (feat/dma-copyout): not reused. */
#define TR_FLAG_SHAKE \
	(1u << 12) /* `shake` below drives the camera shake (booth; an old HE: the
					   * renderer's own crash_tick shake) */

/* Crash camera shake (booth): full at the hit, a quadratic fade to still
 * over TR_SHAKE_FRAMES40 40 Hz frames of crash time (0.4 s at any panel
 * rate) -- the packet's `shake`, from crash_tick (inline: the host tests
 * and the renderer see the same curve). */
#define TR_SHAKE_FRAMES40 16u
static inline uint8_t tr_crash_shake(uint32_t t40)
{
	uint32_t left = t40 < TR_SHAKE_FRAMES40 ? TR_SHAKE_FRAMES40 - t40 : 0u;

	return (uint8_t)((255u * left * left + TR_SHAKE_FRAMES40 * TR_SHAKE_FRAMES40 / 2u) /
	                 (TR_SHAKE_FRAMES40 * TR_SHAKE_FRAMES40));
}

/* tr_frame_in_t.crash_kind values: what the runner hit. */
#define TR_CRASH_KIND_LOW  1u /* a low obstacle (should have jumped) */
#define TR_CRASH_KIND_HIGH 2u /* a high obstacle (should have ducked) */
#define TR_CRASH_KIND_WIRE 3u /* a live wire, either height (ents[crash_ent].low tells which) */

/* tr_frame_in_t.banner values. */
#define TR_BANNER_NONE         0u
#define TR_BANNER_STAND        1u
#define TR_BANNER_STEP_BACK    2u
#define TR_BANNER_ATTRACT      3u
#define TR_BANNER_GAME_OVER    4u
#define TR_BANNER_CHECK_CAMERA 5u

/* One wire entity: 8 B. kind 0 free, 1 obstacle, 2 pickup, 3 live wire
 * (P4b; `low` as for an obstacle) -- mirrors tr_entity_kind_t (state.h) as a
 * plain byte, never the enum itself. A renderer older than P4b (only met in
 * the dev loop: a new HE over an old LAUNCHed renderer) draws a live run's
 * kind 3 as the plain obstacle of its height, but not a wire crash right: it
 * holds only kind 1 at the runner's front (the wire is drawn inside the
 * runner, the pre-P6 ghost) and reads crash_kind 3 as a high hit (a low
 * wire's sparks burst at head height). No flag or version bump for that. */
typedef struct {
	uint8_t kind, lane, low, pad;
	int16_t y, pad2;
} tr_pkt_ent_t;
_Static_assert(sizeof(tr_pkt_ent_t) == 8, "tr_pkt_ent_t must be 8 B on the wire");

/* M55 -> A32 game snapshot, 172 B (P16 + P15; the booth's shake in a pad byte). Field-by-field copy target for
 * tr_game_t -- see tr_frame_in_from_game() below.
 *
 * The crash_* and phase fields (P6/P7) were appended into what used to be
 * the `in` block's pad2 (152 -> 160 B; every block offset unchanged), and
 * all-zero means exactly the old behaviour: no crash, phase 0. So an old
 * renderer ignores them and a new renderer reading an old HE's zeros draws
 * what it always drew -- TR_MBOX_VERSION stays 1. */
typedef struct {
	uint32_t tick, score, rng;
	uint32_t flags;                               /* TR_FLAG_* */
	uint8_t  lane, air_ticks, duck_ticks, banner; /* banner: TR_BANNER_* */
	int16_t  track_h;
	/* With TR_FLAG_CRASH: the fraction of a 40 Hz frame past crash_tick,
	 * Q0.16 (P3d) -- a 30 Hz stream's crash time is 4/3 of a 40 Hz frame a
	 * frame, so crash_tick alone steps 1, 1, 2. 0 from an old HE (the old
	 * pad) or at 40 Hz. */
	uint16_t     crash_frac;
	tr_pkt_ent_t ents[16]; /* TR_MAX_ENTITIES (state.h); every slot, FREE included */
	/* With TR_FLAG_CRASH: frames since the fatal tick (0 = the fatal tick
	 * itself, .. TR_CRASH_TICKS - 1), the ents[] slot that was hit, its
	 * lane and TR_CRASH_KIND_*. All 0 otherwise. */
	uint8_t crash_tick, crash_ent, crash_lane, crash_kind;
	/* With TR_FLAG_PHASE: sub-tick phase, Q0.16 -- how far past `tick`
	 * toward tick + 1 this frame shows the world (play and attract run
	 * slower than one tick per frame, state.h TR_GAME_PACE_Q8; the scene
	 * interpolates scroll and entities). */
	uint16_t phase;
	/* Game steps a frame, Q.8 (128 = 0.5x); 0 from an old HE (and the HE's
	 * crash frames) = the refresh's own 40 Hz frames (r3d_scene.c
	 * frame_k). The scene scales its per-frame motion (particles, scarf
	 * wind) by it. One step a frame -- the booth ramp's cap at 30 Hz -- is
	 * sent as 255, never 256 wrapped to 0 (ramp.h tr_ramp_pace_q8()). */
	uint8_t pace_q8;
	/* The panel refresh this frame is shown for, Hz (P3d, TR_PANEL_HZ): the
	 * scene integrates its easing, blends, landing dip and particles over
	 * 40 / hz 40 Hz frames, so 30 and 40 Hz look the same in real time.
	 * 0 from an old HE (the old pad3 byte) = 40; above 40 is taken as 40
	 * (the scene never eases less than a 40 Hz frame's worth). Per panel
	 * refresh: a packet the A32 skips or renders late still eases by one
	 * refresh, so the independence holds between panel rates, not across
	 * dropped frames. */
	uint8_t hz;
	/* P16, with TR_FLAG_CHAR (appended into what was the `in` block's
	 * pad2: an old renderer ignores them, a new one reading an old HE sees
	 * no flag and draws Probe with no reactions). character: TR_CHAR_*.
	 * react: TR_REACT_* of the latest event, react_side which way it went by
	 * (-1 left, +1 right, as int8), react_seq +1 per event (the renderer
	 * picks attract flourishes from it), react_ms its age (saturating).
	 * With TR_FLAG_IDLE: idle_ms, how long the runner has stood idle. */
	uint8_t  character, react, react_side, react_seq;
	uint16_t react_ms, idle_ms;
	/* With TR_FLAG_ZONE (P15, src/game/zone.h): the world zone the runner
	 * is in, and the model y of the gate to the next one coming down the
	 * track (TR_ZONE_NO_GATE: none) -- the scene draws the gate and blends
	 * the look from these two alone. Appended past P16's 168 B into the
	 * `in` block's pad2 (every block offset unchanged): an old renderer
	 * never sees them; a new renderer under an old HE sees no flag and
	 * draws zone 0, the board it always drew. TR_MBOX_VERSION stays 1
	 * for P16 + P15 as for P6/P7: flag-gated appends, block offsets
	 * unchanged. (P15 alone had zone at 160 / TR_FLAG_ZONE bit 8; that
	 * layout never shipped -- HE and renderer ship as a pair.) */
	uint8_t zone;
	/* With TR_FLAG_SHAKE (booth, the old zone_pad byte): the camera shake's
	 * amplitude, 0 still .. 255 the renderer's full jolt (r3d_scene.h
	 * TR_SCENE_SHAKE_PX); the renderer moves the camera only. Without the
	 * flag (an old HE) the renderer keeps its own crash_tick shake. */
	uint8_t shake;
	int16_t gate_y;
} tr_frame_in_t;
_Static_assert(sizeof(tr_frame_in_t) == 172,
               "tr_frame_in_t layout drifted -- see file header note");
_Static_assert(offsetof(tr_frame_in_t, character) == 160,
               "P16 fields follow hz (the old pad2 bytes)");
_Static_assert(offsetof(tr_frame_in_t, react_ms) == 164 && offsetof(tr_frame_in_t, idle_ms) == 166,
               "P16 fields: 4 bytes, then two uint16");
_Static_assert(offsetof(tr_frame_in_t, zone) == 168 && offsetof(tr_frame_in_t, gate_y) == 170,
               "P15 zone fields follow the P16 fields (the in block's pad2 bytes)");
_Static_assert(offsetof(tr_frame_in_t, shake) == 169, "the booth's shake is the old zone_pad byte");
_Static_assert(offsetof(tr_frame_in_t, crash_frac) == 22,
               "crash_frac is the old pad after track_h");
_Static_assert(offsetof(tr_frame_in_t, crash_tick) == 152,
               "crash fields must follow ents[] (old pad2 bytes)");
_Static_assert(offsetof(tr_frame_in_t, pace_q8) == 158, "pace_q8 is the old pad3's first byte");
_Static_assert(offsetof(tr_frame_in_t, hz) == 159, "hz is the old pad3's second byte");

/* A32 -> M55 per-frame stats payload (28 B): the fields after out_seq in
 * tr_mbox_t, bundled so publish/take can move them in one copy. Not a
 * standalone wire block of its own -- tr_mbox_publish_out()/take_out() write
 * straight into/out of the matching tr_mbox_t members. */
typedef struct {
	uint32_t fb;
	uint32_t ticks0, ticks1;
	uint32_t tris, dropped, frames, heartbeat;
} tr_frame_out_t;
_Static_assert(sizeof(tr_frame_out_t) == 28,
               "tr_frame_out_t must mirror the 7 out_* fields exactly");

typedef struct {
	/* +0x000 identity, M55 writes once per boot */
	uint32_t magic, version, m55_boot_count, pad0[13];
	/* +0x040 M55 -> A32 */
	uint32_t      in_seq; /* incremented AFTER in_* + in_fb are written and barrier()'d */
	uint32_t      in_fb;  /* TR_FB_A or TR_FB_B: draw here */
	uint32_t      pad1[14];
	tr_frame_in_t in;       /* +0x080, 172 B */
	uint8_t       pad2[20]; /* pads the `in` block out to +0x140 (see file header note) */
	/* +0x140 A32 -> M55 */
	uint32_t out_seq; /* == in_seq of the frame now complete in out_fb */
	uint32_t out_fb;
	uint32_t out_ticks0, out_ticks1; /* CNTVCT ticks each core spent on this frame */
	uint32_t out_tris, out_dropped, out_frames, out_heartbeat, pad3[8];
	/* +0x180 stub control */
	uint32_t ctrl_cmd; /* 0 none 1 LAUNCH 2 HALT (consumer clears) */
	uint32_t ctrl_entry, ctrl_len, ctrl_crc;
	uint32_t stub_state;       /* 0 PARKED 1 RUNNING 2 FAULT */
	uint32_t stub_core1_state; /* 0 never started 1 PARKED 2 RUNNING */
	uint32_t stub_heartbeat0, stub_heartbeat1, pad4[8];
	/* +0x1C0 fault record */
	uint32_t fault_core, fault_code, dfsr, dfar, ifsr, ifar, lr, pad5[9];
} tr_mbox_t;

_Static_assert(sizeof(tr_mbox_t) == 0x200, "tr_mbox_t total size drifted -- see file header note");
_Static_assert(offsetof(tr_mbox_t, in_seq) == 0x040, "M55->A32 header block moved");
_Static_assert(offsetof(tr_mbox_t, in) == 0x080, "in-payload block moved");
_Static_assert(offsetof(tr_mbox_t, out_seq) == 0x140,
               "A32->M55 block moved -- see file header note");
_Static_assert(offsetof(tr_mbox_t, ctrl_cmd) == 0x180,
               "stub control block moved -- see file header note");
_Static_assert(offsetof(tr_mbox_t, fault_core) == 0x1C0,
               "fault record block moved -- see file header note");

/* Field-by-field snapshot of `g` into `out` (see the header comment on why
 * tr_game_t is never embedded). `banner`, `attract_active` and `paused` are
 * display/mode state tr_game_t doesn't carry, passed in by the caller.
 * out->track_h and out->phase are left 0 -- the caller sets panel geometry
 * and the attract sub-tick phase separately. A crashed run (g->crashed)
 * sets TR_FLAG_CRASH and the crash_* fields. */
void tr_frame_in_from_game(tr_frame_in_t   *out,
                           const tr_game_t *g,
                           uint8_t          banner,
                           bool             attract_active,
                           bool             paused);

/* The P16 fields: sets TR_FLAG_CHAR, character (out of range: Probe), the
 * reaction from `r`; `idle` sets TR_FLAG_IDLE with idle_ms from idle_us
 * (saturating), else idle_ms 0. Call after tr_frame_in_from_game(). */
void tr_frame_in_p16(tr_frame_in_t    *out,
                     uint8_t           character,
                     const tr_react_t *r,
                     bool              idle,
                     uint32_t          idle_us);

/* The zone fields of `out` from `z` (P15), with TR_FLAG_ZONE. The HE's
 * packet only: tr_frame_in_from_game() leaves them zero and the flag off. */
void tr_frame_in_set_zone(tr_frame_in_t *out, const tr_zone_t *z);

/* TR_CRASH_KIND_* of what ended a crashed run (0 when g did not crash):
 * the frame packet's crash_kind, and the sound's CRASH parameter. */
uint8_t tr_game_crash_kind(const tr_game_t *g);

/* M55 side: publish a new `in` snapshot. Copies payload + fb, barrier(),
 * bumps in_seq, barrier(). */
void tr_mbox_publish_in(volatile tr_mbox_t  *m,
                        const tr_frame_in_t *in,
                        uint32_t             fb,
                        void (*barrier)(void));

/* A32 side: take the `in` snapshot if it's newer than last_seq. Returns
 * false with out/fb/seq untouched if there's nothing new, or if the writer
 * started publishing again mid-copy (torn read). */
bool tr_mbox_take_in(const volatile tr_mbox_t *m,
                     uint32_t                  last_seq,
                     tr_frame_in_t            *out,
                     uint32_t                 *fb,
                     uint32_t                 *seq,
                     void (*barrier)(void));

/* A32 side: publish out_* stats for the frame identified by `seq` (the
 * in_seq this frame completed). */
void tr_mbox_publish_out(volatile tr_mbox_t   *m,
                         const tr_frame_out_t *out,
                         uint32_t              seq,
                         void (*barrier)(void));

/* M55 side: take out_* if newer than last_seq; same torn-read protocol as
 * tr_mbox_take_in(). */
bool tr_mbox_take_out(const volatile tr_mbox_t *m,
                      uint32_t                  last_seq,
                      tr_frame_out_t           *out,
                      uint32_t                 *seq,
                      void (*barrier)(void));

#endif /* TR_MBOX_H */
