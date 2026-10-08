/* a32/renderer/render.c -- see render.h. Build with RENDER_A32=1 for the
 * renderer image (buffers at their SRAM1 addresses, CNTVCT timing, NEON
 * copy); without it (host test) buffers are static and ticks read 0. */
#include "render.h"

#include <stddef.h>
#include <string.h>

#include "../../src/render/atlas.h"
#include "../../src/render/cam_pip.h"
#include "../../src/render/panel_rot.h"
#include "../../src/render/r3d.h"
#include "../../src/render/r3d_scene.h"
#include "../../src/render/sprite.h"
#include "../../tests/host/tr_golden_dl.h"
#include "../common/crc32.h"
#include "../common/stub_abi.h"
#include "../../src/ipc/tr_cam_view.h"
#include "../../src/ipc/tr_hp_dbg.h"
#include "../../src/ipc/tr_memmap.h"
#include "../../src/ipc/tr_pslot.h"

#define BAND_PX (TR_R3D_W * TR_BAND_H)

/* The frame's rotation (panel_rot.h; in->rotation, from the display's
 * mount-rotation): the bands are always drawn portrait, 720 px wide, into the
 * cores' cached scratch, and only the copy out to the framebuffer turns them
 * (a 1280 x 720 window for 90 / 270 = the same RENDER_FB_BYTES). Set once a
 * frame by render_front_begin(), before any band is claimed. The golden
 * build's CRCs are of the portrait layout, so it always draws rotation 0. */
static int frame_rot;
_Static_assert(TR_R3D_W == TR_ROT_PORTRAIT_W && TR_R3D_H == TR_ROT_PORTRAIT_H,
               "panel_rot.h: the portrait frame");
_Static_assert(TR_BAND_H % 8 == 0 && TR_BAND_H <= 32,
               "panel_rot.h's NEON blit takes up to four 8-row groups");

/* The A32 never writes the TF-A MHU0 window, and nothing here skips it any
 * more: no framebuffer may contain it (FB B moved to SRAM1). */
_Static_assert(STUB_MHU0_WINDOW == TR_MHU0_WINDOW_LO &&
                   STUB_MHU0_WINDOW + STUB_MHU0_WINDOW_SIZE == TR_MHU0_WINDOW_HI,
               "tr_mbox.h and stub_abi.h disagree on the MHU0 window");
_Static_assert(TR_FB_CLEAR_OF_MHU0(TR_FB_A) && TR_FB_CLEAR_OF_MHU0(TR_FB_B),
               "a framebuffer overlaps the TF-A MHU0 window [0x02380000, 0x02381000)");
_Static_assert(TR_FB_B + TR_FB_SIZE <= 0x027DE000u, "FB B runs into TF-A RW (0x027DE000)");
_Static_assert(RENDER_FB_BYTES == TR_FB_SIZE, "framebuffer size");
/* The HE's HUD buffer (layer 2) sits above everything the renderer maps in SRAM0. */
_Static_assert(TR_HUD_FB >= STUB_MHU0_WINDOW + STUB_MHU0_WINDOW_SIZE &&
                   TR_HUD_FB + TR_HUD_FB_SIZE <= STUB_EARLY_PARK,
               "HUD buffer overlaps the MHU0 window or the stub's early park page");

#if RENDER_A32
#include <arm_neon.h>

/* a32-renderer plan sec 4: SRAM1 MiB 1 (DL, bins) and SRAM0 MiB 2
 * 0x022xxxxx (setup records, per core a z + a colour band), both Normal
 * WB-WA S=1 in the renderer table (core 1 switches to it too). */
#define DL    ((tr_dl_t *)TR_MEM_A32_DL)
#define BINS  ((uint16_t (*)[TR_BIN_MAX])TR_MEM_A32_BINS)
#define SETUP ((tr_tri_setup_t *)TR_MEM_A32_SETUP)
/* Scene part 2's DL (core 1), SRAM0 MiB 3 below the MHU0 window, mapped
 * WB-WA S=1 through 4 KiB pages (renderer.c). */
#define DL1 ((tr_dl_t *)TR_MEM_A32_DL1)
/* DL1 must leave room for the HP's NPU arena (tr_memmap.h TR_MEM_NPU_ARENA,
 * docs/superpowers/specs/2026-09-24-npu-body-control-design.md sec 2/9)
 * before the sound ring / MHU0 window page. */
_Static_assert(TR_MEM_A32_DL1 + sizeof(tr_dl_t) <= TR_MEM_NPU_ARENA, "DL1 runs into the NPU arena");
_Static_assert(TR_MEM_NPU_ARENA + TR_MEM_NPU_ARENA_SIZE <= TR_MEM_ARING,
               "NPU arena runs into the MHU0 window / sound ring");
#define ZBAND(c) ((uint16_t *)TR_MEM_A32_BANDS + (c) * 2u * BAND_PX)
#define CBAND(c) (ZBAND(c) + BAND_PX)
_Static_assert(TR_MEM_A32_DL >= TR_FB_A + TR_FB_SIZE &&
                   TR_MEM_A32_DL + sizeof(tr_dl_t) <= TR_MEM_A32_SETUP,
               "DL must sit between FB A and the setup records (SRAM0 MiB 1 tail)");
_Static_assert(TR_MEM_A32_IMG_END <= TR_MEM_A32_BINS, "renderer image + .bss overrun the bins");
_Static_assert(TR_MEM_A32_BINS + TR_BANDS * TR_BIN_MAX * 2u <= TR_MEM_A32_STACKS,
               "bins overrun the stacks");
_Static_assert(TR_MEM_A32_SETUP + sizeof(tr_tri_setup_t) * TR_DL_MAX_TRIS <= TR_MEM_A32_BANDS,
               "setup records overrun the bands");
_Static_assert(TR_MEM_A32_BANDS + RENDER_CORES * 4u * BAND_PX <= TR_MEM_A32_ZTEX,
               "bands overrun the zone textures");
_Static_assert(TR_MEM_A32_ZTEX + TR_MEM_A32_ZTEX_SIZE <= TR_MEM_A32_ZIDX,
               "zone textures overrun the zone indices");
_Static_assert(TR_MEM_A32_ZIDX + TR_MEM_A32_ZIDX_SIZE <= TR_MEM_A32_DL1,
               "zone indices overrun SRAM0 MiB 2");

static inline uint32_t ticks(void)
{
	uint32_t lo, hi;

	__asm__ volatile("isb\n\tmrrc p15, 1, %0, %1, c14" : "=r"(lo), "=r"(hi)::"memory");
	(void)hi;
	return lo;
}

static inline void copy16(uint16_t *d, const uint16_t *s)
{
	vst1q_u16(d, vld1q_u16(s));
}
static void pip_barrier(void)
{
	__asm__ volatile("dsb sy" ::: "memory");
}
#else
static tr_dl_t dl_mem;
#if !RENDER_DL_GOLDEN
static tr_dl_t dl1_mem;
#endif
static uint16_t       bins_mem[TR_BANDS][TR_BIN_MAX];
static tr_tri_setup_t setup_mem[TR_DL_MAX_TRIS];
static uint16_t       band_mem[RENDER_CORES][2 * BAND_PX];
#define DL       (&dl_mem)
#define DL1      (&dl1_mem)
#define BINS     bins_mem
#define SETUP    setup_mem
#define ZBAND(c) band_mem[c]
#define CBAND(c) (band_mem[c] + BAND_PX)

static inline uint32_t ticks(void)
{
	return 0;
}
static inline void copy16(uint16_t *d, const uint16_t *s)
{
	memcpy(d, s, 16);
}
static void pip_barrier(void)
{
}
#endif

render_stats_t      render_stats;
int                 render_hud = !RENDER_DL_GOLDEN;
render_core_stats_t render_core_stats[RENDER_CORES] __attribute__((aligned(64)));
const uint32_t      render_golden_crc                = TR_GOLDEN_RASTER_CRC;
const uint32_t      render_golden_band_crc[TR_BANDS] = TR_GOLDEN_BAND_CRC;

static uint32_t counts[TR_BANDS];
static tr_bg_t  frame_bg;              /* this frame's band background */
static uint32_t hud_score, hud_banner; /* this frame's HUD, from `in` */
static uint32_t hud_flags; /* this frame's TR_FLAG_* (TR_FLAG_HUD_L2: the HE draws the HUD) */
#if RENDER_DL_GOLDEN
static uint16_t tex0[TR_TEX_DIM * TR_TEX_DIM];
#else
static tr_scene_t    scene;     /* .bss: WB S=1 on the A32; stepped by core 0 only */
static tr_frame_in_t scene_in;  /* this frame's input, for both scene parts */
static tr_cam_t      scene_cam; /* part 1's camera (== part 2's), for the background */
#endif
static uint32_t front_t0;

void render_init(void)
{
#if RENDER_DL_GOLDEN
	tr_golden_tex_init(tex0);
	tr_r3d_tex[0] = tex0;
#else
	tr_scene_init(&scene); /* also points tr_r3d_tex[] at the scene textures */
#endif
}

/* The mode hook: the frame's DL and band background. */
static void build_dl(const tr_frame_in_t *in, tr_dl_t *dl, tr_bg_t *bg)
{
#if RENDER_DL_GOLDEN
	static const tr_bg_t gbg = TR_GOLDEN_BG;

	(void)in;
	memcpy(dl->tri, tr_golden_dl, sizeof(tr_golden_dl));
	dl->n         = TR_GOLDEN_DL_N;
	*bg           = gbg;
	tr_dl_dropped = 0;
#else
	tr_cam_t cam;

	tr_scene_step(&scene, in);
	tr_scene_build(&scene, in, &cam, dl);
	tr_scene_bg(in, &cam, bg);
	tr_scene_bg_flash(in, bg);
#endif
}

/* HUD geometry: the M55 renderer's (src/render/render.c draw_hud /
 * tr_render_banner) -- score digits top-left, banner plate top-centre. */
#define HUD_X0          4
#define HUD_Y0          8
#define HUD_DIGIT_GAP   2
#define BANNER_W        480
#define BANNER_H        64
#define BANNER_X0       ((TR_R3D_W - BANNER_W) / 2)
#define RGB565(r, g, b) ((uint16_t)(((r) & 0xF8) << 8 | ((g) & 0xFC) << 3 | (b) >> 3))
#define COLOR_PLATE     RGB565(40, 40, 48)
#define COLOR_COPPER    RGB565(176, 100, 40)

static const tr_sprite_t *const digit_spr[10] = {
	&tr_spr_digit_0, &tr_spr_digit_1, &tr_spr_digit_2, &tr_spr_digit_3, &tr_spr_digit_4,
	&tr_spr_digit_5, &tr_spr_digit_6, &tr_spr_digit_7, &tr_spr_digit_8, &tr_spr_digit_9,
};
/* Indexed by TR_BANNER_* (tr_mbox.h); NULL = none. */
static const tr_sprite_t *const banner_spr[6] = {
	NULL,
	&tr_spr_banner_stand,
	&tr_spr_banner_step_back,
	&tr_spr_banner_attract,
	&tr_spr_banner_game_over,
	&tr_spr_banner_check_camera,
};

/* Band-local rectangle fill, clipped to the band's rows. */
static void band_fill(uint16_t *cband, int y_lo, int x, int y, int w, int h, uint16_t c)
{
	int r0 = y - y_lo > 0 ? y - y_lo : 0, r1 = y + h - y_lo < TR_BAND_H ? y + h - y_lo : TR_BAND_H;

	for (int r = r0; r < r1; r++) {
		for (int i = 0; i < w; i++) {
			cband[r * TR_R3D_W + x + i] = c;
		}
	}
}

/* Score + banner into the cached colour band of rows [y_lo, y_lo + 32):
 * tr_sprite_blit clips each sprite to the band. Only the top 64 rows. */
static void hud_band(uint16_t *cband, int y_lo)
{
	const tr_sprite_t *ban = hud_banner < 6u ? banner_spr[hud_banner] : NULL;

	if (y_lo >= BANNER_H) {
		return;
	}
	if (ban != NULL) {
		band_fill(cband, y_lo, BANNER_X0, 0, BANNER_W, BANNER_H, COLOR_PLATE);
		band_fill(cband, y_lo, BANNER_X0 + 14, BANNER_H - 9, BANNER_W - 29, 3, COLOR_COPPER);
		tr_sprite_blit(cband,
		               TR_R3D_W,
		               TR_R3D_W,
		               TR_BAND_H,
		               BANNER_X0,
		               (BANNER_H - ban->h) / 2 - y_lo,
		               ban,
		               tr_spr_palette);
	}

	uint8_t d[TR_SCORE_MAX_DIGITS];
	int     n = tr_score_to_digits(hud_score, d), x = HUD_X0;

	for (int i = 0; i < n; i++) {
		tr_sprite_blit(cband,
		               TR_R3D_W,
		               TR_R3D_W,
		               TR_BAND_H,
		               x,
		               HUD_Y0 - y_lo,
		               digit_spr[d[i]],
		               tr_spr_palette);
		x += digit_spr[d[i]]->w + HUD_DIGIT_GAP;
	}
}

/* Video area (maintainer ruling "Half / half", supersedes fix rounds 8-10's
 * landscape fill): rows [TR_VID_Y0, TR_R3D_H) (cam_pip.h) of the SAME
 * 720x1280 framebuffer, dark background, no border. The camera at native
 * 1:1, turned upright (src/vision/cam_rot.h), with its skeleton on top. The
 * rotation is whatever the HP applied and published (tr_cam_view_t.rotate),
 * so image and keypoints always agree, and it picks the layout:
 *   - 90 / 270, the sensor on its side: 400x640 portrait at x 160..559; the
 *     left 160-px strip holds the LEFT ARM / RIGHT ARM / BOTH ARMS / DUCK
 *     lamps stacked, the right one the "CAMERA / NPU Hz" label;
 *   - 0, the camera upright and LANDSCAPE (the arm controls want the wider
 *     field of view -- arms reach sideways): 640x400 at x 40..679, rows
 *     120..519, letterboxed 120 rows above and below (not stretched). The
 *     lamps run along the top letterbox and the label along the bottom one,
 *     so nothing is drawn over the picture.
 *
 * Split like the 3D bands (fix round 10's lesson, 35.9 ms when it was one
 * uncached pass): render_video_band() is claimable band work, either core,
 * rendered into the core's cached CBAND scratch -- background, camera,
 * lamps, label -- then one write-only NEON copy out. Only the skeleton (a
 * few hundred px) is left to render_video_overlay(), core 0, after the
 * bands. Everything frame-constant the bands need (the camera view, the lamp
 * states, the label text) is taken ONCE a frame by video_frame_state(),
 * core 0, before any band is claimed, so both cores draw the same frame. */
#ifndef TR_CAM_PIP_ENABLE
#define TR_CAM_PIP_ENABLE 1 /* compile-time switch: 0 leaves the video area untouched */
#endif
#define COLOR_PANEL_BG  RGB565(12, 14, 18)    /* the video area's dark background */
#define COLOR_KP        RGB565(80, 255, 96)   /* skeleton: the HUD's accent green */
#define COLOR_LAMP_ON   RGB565(255, 210, 40)  /* intent lamp, lit */
#define COLOR_LAMP_OFF  RGB565(52, 54, 62)    /* intent lamp, unlit: visible on the dark strip */
#define COLOR_LABEL     RGB565(235, 245, 235) /* label/caption text */
#define VID_STRIP_W     160                   /* portrait: each side strip, (720 - 400) / 2 */
#define LAND_BAND_H     ((TR_VID_H - TR_CAM_SENSOR_H) / 2) /* landscape: each letterbox band, 120 */
#define LAND_LAMP_SQ    56                                 /* landscape lamp square, px */
#define LAMP_CELL_H     (TR_VID_H / 4)        /* four lamps stacked down the left strip */
#define LAMP_SQ         96                    /* lamp square, px */
#define LAMP_CAP_SCALE  4                     /* caption: 3x5 font at 4x -> 20 px tall */
#define LAMP_HOLD_TICKS 10

#if TR_CAM_PIP_ENABLE
#if RENDER_A32
#define CAM_VIEW_ADDR  ((const volatile tr_cam_view_t *)TR_MEM_CAM_VIEW)
#define PIP_PSLOT_ADDR ((const volatile tr_pslot_t *)TR_MEM_PSLOT)
#define HP_DBG_ADDR    ((const volatile hp_dbg_t *)TR_MEM_HP_DBG)
/* DCIMVAC: Data Cache line Invalidate by VA to PoC (ARMv7-A, Cortex-A32) --
 * the camera writes CAM_POOL by DMA, so a cached A32 read of it needs this
 * before every access or it can read stale, pre-DMA bytes. 32 B lines;
 * walked from a line-aligned start so all of [addr, addr+bytes) is covered.
 * Always safe here: the A32 never writes CAM_POOL, so no line is ever dirty. */
static inline void dcache_inval_range(const void *addr, uint32_t bytes)
{
	uintptr_t a   = (uintptr_t)addr & ~(uintptr_t)31u;
	uintptr_t end = (uintptr_t)addr + bytes;

	for (; a < end; a += 32u) {
		__asm__ volatile("mcr p15, 0, %0, c7, c6, 1" ::"r"(a) : "memory");
	}
}
#else
/* Host build: TR_MEM_CAM_VIEW/TR_MEM_PSLOT are target addresses, not host
 * memory (the host tests #include this file) -- static host buffers instead,
 * left zeroed: the seqlock's magic check then rejects, and the video area
 * draws its no-camera background, as on silicon before hp_vision publishes. */
static tr_cam_view_t host_cam_view_mem;
static tr_pslot_t    host_pslot_mem;
static hp_dbg_t      host_hp_dbg_mem;
#define CAM_VIEW_ADDR  ((const volatile tr_cam_view_t *)&host_cam_view_mem)
#define PIP_PSLOT_ADDR ((const volatile tr_pslot_t *)&host_pslot_mem)
#define HP_DBG_ADDR    ((const volatile hp_dbg_t *)&host_hp_dbg_mem)
static inline void   dcache_inval_range(const void *addr, uint32_t bytes)
{
	(void)addr;
	(void)bytes;
}
#endif
#endif /* TR_CAM_PIP_ENABLE */

/* A drawing target: screen pixel (x, y) lives at px[(y - y0) * TR_R3D_W + x],
 * and only [x0, x1) x [y0, y1) is drawn -- a band's cached scratch (y0 its
 * first screen row) or the framebuffer itself. */
typedef struct {
	uint16_t *px;
	int       x0, x1, y0, y1;
	int       rot; /* -1: px is a band's scratch; else px is the framebuffer, turned by rot */
} vcv_t;

static void cv_rect(const vcv_t *cv, int x, int y, int w, int h, uint16_t c)
{
	int xa = x > cv->x0 ? x : cv->x0, xb = x + w < cv->x1 ? x + w : cv->x1;
	int ya = y > cv->y0 ? y : cv->y0, yb = y + h < cv->y1 ? y + h : cv->y1;

	if (cv->rot >= 0) {
		for (int yy = ya; yy < yb; yy++) {
			for (int xx = xa; xx < xb; xx++) {
				cv->px[tr_rot_idx(cv->rot, TR_R3D_H, xx, yy)] = c;
			}
		}
		return;
	}
	for (int yy = ya; yy < yb; yy++) {
		uint16_t *d = &cv->px[(uint32_t)(yy - cv->y0) * TR_R3D_W];

		for (int xx = xa; xx < xb; xx++) {
			d[xx] = c;
		}
	}
}

static void video_dot(const vcv_t *cv, int x, int y, uint16_t color)
{
	cv_rect(cv, x - 1, y - 1, 3, 3, color);
}

/* 2 px integer DDA line (Bresenham), clipped by the canvas -- the skeleton
 * bones. Two 1-px passes offset by (1,0)/(0,1) depending on slope. */
static void video_line(const vcv_t *cv, int x0, int y0, int x1, int y1, uint16_t color)
{
	int dx = x1 > x0 ? x1 - x0 : x0 - x1, sx = x0 < x1 ? 1 : -1;
	int dy  = y1 > y0 ? y0 - y1 : y1 - y0,
	    sy  = y0 < y1 ? 1 : -1; /* dy negative, Bresenham's own convention */
	int err = dx + dy, steep = dx < -dy;

	for (;;) {
		video_dot(cv, x0, y0, color);
		video_dot(cv, x0 + (steep ? 1 : 0), y0 + (steep ? 0 : 1), color);
		if (x0 == x1 && y0 == y1) {
			break;
		}
		int e2 = 2 * err;

		if (e2 >= dy) {
			err += dy;
			x0 += sx;
		}
		if (e2 <= dx) {
			err += dx;
			y0 += sy;
		}
	}
}

/* The 12-bone MoveNet/COCO skeleton (design doc sec 6) -- both ends must
 * clear TR_POSE_KP_MIN (tr_cam_pip_map_kp's own gate) before a bone draws. */
static const uint8_t bones[12][2] = {
	{ TR_KP_LSHO, TR_KP_RSHO }, { TR_KP_LSHO, TR_KP_LELB }, { TR_KP_LELB, TR_KP_LWRI },
	{ TR_KP_RSHO, TR_KP_RELB }, { TR_KP_RELB, TR_KP_RWRI }, { TR_KP_LSHO, TR_KP_LHIP },
	{ TR_KP_RSHO, TR_KP_RHIP }, { TR_KP_LHIP, TR_KP_RHIP }, { TR_KP_LHIP, TR_KP_LKNE },
	{ TR_KP_LKNE, TR_KP_LANK }, { TR_KP_RHIP, TR_KP_RKNE }, { TR_KP_RKNE, TR_KP_RANK },
};

/* Minimal 3x5 block font (fix round 8 item 4, maintainer ruling: the video
 * panel draws its own "CAMERA 640x400 - NPU xx Hz" label -- the perf
 * panel's own line for it, fix round 7 item 5, is retired, see hud.h's
 * TR_PERF_LINES comment). Bench-legible at 2x, not typography.
 *
 * fix round 12 (silicon finding: the on-panel label rendered as
 * "NPU 2 .  HZ" -- blank glyph slots after "2" and after ".", although
 * loop_hz_x10 was genuinely 251..266 (25.1..26.6 Hz). Root cause: digits
 * '3' and '5' were missing from this table -- video_glyph() silently draws
 * NOTHING for an unmatched char (no fallback box, no assert), so a real
 * "25.1Hz" rendered as a gap where the '5' belongs, not a wrong digit; the
 * earlier round-9/10/11 checks that confirmed "26.6Hz" renders correctly
 * happened to pick values with no '3' or '5' in them and missed this. Every
 * digit 0-9 is in the table now (added 3 and 5), plus 'Z' (the label's own
 * "Hz" only ever used lowercase 'z', already present, but a caller COULD
 * reasonably ask for "HZ") -- tests/host/test_r3d_cam_pip.c's own new
 * glyph-coverage test renders "0123456789.HZ" through this table and
 * asserts no glyph comes back blank, so a future addition that misses one
 * character fails a test instead of shipping a silent gap again.
 *
 * fix round 13 (silicon finding: "NPU" on the real label read as "KPU" --
 * a DIFFERENT bug from round 12's blank-glyph one: 'N' was present, but
 * its 5-row pattern differed from 'K' in only ONE row (both #.#/#.#/#.#
 * on three of five rows), too close at this resolution to read apart. 'N'
 * redrawn with a two-step diagonal -- but the redraw itself had a second
 * defect (round 14 finding, below), and the row-distinctness test this
 * comment describes was blind to it: it counts DIFFERING rows against
 * 'K', never checks that 'N' is a correctly-formed N on its own, so a
 * wrong-but-still->=3-rows-different glyph passed clean. A blank-glyph
 * check alone can't catch a wrong-but-non-blank glyph either, so
 * tests/host/test_a32_font.c also renders every letter pair this font
 * could plausibly confuse (starting with N/K) and widens its coverage
 * string to the label's own full text ("CAMERA 640x400 - NPU").
 *
 * fix round 14 (silicon finding: 'N' still read wrong -- the round-13
 * redraw dropped the LEFT column entirely on one of the two diagonal rows
 * (".##"), breaking the left vertical stroke every real N keeps for its
 * full height). Round 14's own redraw CLAIMED both edges were now lit on
 * every row, but the committed table still broke an edge on two rows
 * (".##" and "##."), AND the reference bitmap added to
 * tests/host/test_a32_font.c that round was typed to match the same
 * wrong glyph instead of an independently-checked correct one, so it
 * passed clean while pinning the bug in place -- caught only by a THIRD
 * silicon boot (round 15).
 *
 * fix round 15: edges on all 5 rows is a checked invariant
 * (tests/host/test_a32_font.c).
 *
 * Polish round (silicon, the half layout: "NPU" STILL read as "KPU", and
 * "JUMP"'s M as a blob): no 3-px-wide N or M is legible -- a diagonal
 * needs a column of its own. Glyphs are now variable width: N is 4
 * columns (a real diagonal), M 5; every other glyph stays 3. */
typedef struct {
	char c;
	char rows[5][6]; /* 3 columns, or up to 5 for M/N (the glyph's width is strlen(rows[0])) */
} glyph3x5_t;
static const glyph3x5_t font3x5[] = {
	{ 'C', { "###", "#..", "#..", "#..", "###" } },
	{ 'A', { "###", "#.#", "###", "#.#", "#.#" } },
	{ 'M', { "#...#", "##.##", "#.#.#", "#...#", "#...#" } },
	{ 'E', { "###", "#..", "###", "#..", "###" } },
	{ 'R', { "##.", "#.#", "##.", "#.#", "#.#" } },
	{ 'N', { "#..#", "##.#", "#.##", "#..#", "#..#" } },
	{ 'P', { "##.", "#.#", "##.", "#..", "#.." } },
	{ 'U', { "#.#", "#.#", "#.#", "#.#", "###" } },
	{ 'H', { "#.#", "#.#", "###", "#.#", "#.#" } },
	{ 'z', { "###", "..#", ".#.", "#..", "###" } },
	{ 'Z', { "###", "..#", ".#.", "#..", "###" } },
	{ '0', { "###", "#.#", "#.#", "#.#", "###" } },
	{ '2', { "###", "..#", "###", "#..", "###" } },
	{ '3', { "###", "..#", "###", "..#", "###" } },
	{ '5', { "###", "#..", "###", "..#", "###" } },
	{ '4', { "#.#", "#.#", "###", "..#", "..#" } },
	{ '6', { "###", "#..", "###", "#.#", "###" } },
	{ '7', { "###", "..#", ".#.", "#..", "#.." } },
	{ '8', { "###", "#.#", "###", "#.#", "###" } },
	{ '9', { "###", "#.#", "###", "..#", "###" } },
	{ '1', { ".#.", "##.", ".#.", ".#.", "###" } },
	{ 'x', { "#.#", ".#.", ".#.", ".#.", "#.#" } },
	{ '.', { "...", "...", "...", "...", "#.." } },
	{ '-', { "...", "...", "###", "...", "..." } },
	{ ' ', { "...", "...", "...", "...", "..." } },
	{ 'L', { "#..", "#..", "#..", "#..", "###" } },
	{ 'F', { "###", "#..", "###", "#..", "#.." } },
	{ 'T', { "###", ".#.", ".#.", ".#.", ".#." } },
	{ 'I', { "###", ".#.", ".#.", ".#.", "###" } },
	{ 'G', { "###", "#..", "#.#", "#.#", "###" } },
	{ 'J', { "..#", "..#", "..#", "#.#", "###" } },
	{ 'D', { "##.", "#.#", "#.#", "#.#", "##." } },
	{ 'K', { "#.#", "#.#", "##.", "#.#", "#.#" } },
	{ 'B', { "##.", "#.#", "##.", "#.#", "##." } },
	{ 'O', { "###", "#.#", "#.#", "#.#", "###" } }, /* the same bitmap as '0': context tells */
	{ 'S', { "###", "#..", "###", "..#", "###" } }, /* the same bitmap as '5' */
};

static const glyph3x5_t *video_glyph_find(char c)
{
	for (size_t g = 0; g < sizeof(font3x5) / sizeof(font3x5[0]); g++) {
		if (font3x5[g].c == c) {
			return &font3x5[g];
		}
	}
	return NULL;
}

/* Glyph width in font columns (3 for an unknown char: a blank slot). */
static int video_glyph_w(char c)
{
	const glyph3x5_t *g = video_glyph_find(c);

	return g != NULL ? (int)strlen(g->rows[0]) : 3;
}

static void video_glyph(const vcv_t *cv, int x0, int y0, char c, int scale, uint16_t color)
{
	const glyph3x5_t *g = video_glyph_find(c);

	if (g == NULL) {
		return;
	}
	for (int r = 0; r < 5; r++) {
		for (int col = 0; g->rows[r][col] != '\0'; col++) {
			if (g->rows[r][col] == '#') {
				cv_rect(cv, x0 + col * scale, y0 + r * scale, scale, scale, color);
			}
		}
	}
}

static void video_text(const vcv_t *cv, int x0, int y0, const char *s, int scale, uint16_t color)
{
	int x = x0;

	for (; *s; s++) {
		video_glyph(cv, x, y0, *s, scale, color);
		x += (video_glyph_w(*s) + 1) * scale; /* glyph width + a 1-col gap */
	}
}

/* video_text() centred on column cx. */
static void video_text_c(const vcv_t *cv, int cx, int y0, const char *s, int scale, uint16_t color)
{
	int w = -1; /* no gap after the last glyph */

	for (const char *p = s; *p; p++) {
		w += video_glyph_w(*p) + 1;
	}
	video_text(cv, cx - w * scale / 2, y0, s, scale, color);
}

/* This frame's video-area state, taken once by video_frame_state() (core 0,
 * before any band is claimed -- the frame_go barrier publishes it to core 1). */
static tr_cam_view_t vid_cv;      /* the camera view, range-checked; .rotate also sets the layout */
static bool          vid_have_cv; /* vid_cv is safe to read pixels through */
static bool          vid_lamp[4]; /* LEFT, RIGHT, JUMP, DUCK */
static bool          vid_have_hz; /* hp_vision's loop rate is live */
static char          vid_hz[12];  /* "NN.NHz" or "--" */
static char          vid_dims[12]; /* the upright image's size, "400x640" */

/* Unsigned decimal into out, no libc; returns the length. */
static int u_dec(uint32_t v, char *out)
{
	char t[10];
	int  n = 0, k = 0;

	do {
		t[n++] = (char)('0' + v % 10u);
		v /= 10u;
	} while (v != 0u && n < 10);
	while (n > 0) {
		out[k++] = t[--n];
	}
	out[k] = '\0';
	return k;
}

static void video_frame_state(void)
{
#if TR_CAM_PIP_ENABLE
	tr_cam_view_t cv;
	bool          ok = tr_cam_view_read(CAM_VIEW_ADDR, &cv, pip_barrier);

	/* Range-checked BEFORE any pixel byte is read (fix round 12/13's
	 * rule): the buffer lies wholly inside CAM_POOL, and is exactly the
	 * mode every index below assumes -- the rotated reads walk all
	 * TR_CAM_SRC_H rows, whatever a smaller published height would claim. */
	if (ok) {
		uint64_t lo = cv.buf_addr, hi = lo + (uint64_t)cv.width * cv.height;

		ok = lo >= TR_MEM_CAM_POOL && hi <= TR_MEM_CAM_POOL + TR_MEM_CAM_POOL_SIZE &&
		     cv.width == TR_CAM_SRC_W && cv.height == TR_CAM_SRC_H &&
		     (cv.rotate == 0u || cv.rotate == 90u || cv.rotate == 270u);
	}
	if (!ok) {
		cv.rotate = TR_CAM_ROTATE; /* layout of the empty area: this build's default */
	}
	vid_cv      = cv;
	vid_have_cv = ok;

	int n = u_dec((uint32_t)TR_CAM_UP_W(cv.rotate), vid_dims);

	vid_dims[n++] = 'x';
	u_dec((uint32_t)TR_CAM_UP_H(cv.rotate), vid_dims + n);

	/* The HP's loop rate (src/ipc/tr_hp_dbg.h), a seqlock read (fix round
	 * 11): a torn read just shows "--" for one frame. */
	hp_dbg_t dbg;

	vid_have_hz = tr_hp_dbg_read(HP_DBG_ADDR, &dbg, pip_barrier) && dbg.magic == TR_HP_DBG_MAGIC;
	if (vid_have_hz) {
		tr_cam_pip_format_hz(dbg.loop_hz_x10, vid_hz);
	} else {
		vid_hz[0] = vid_hz[1] = '-', vid_hz[2] = '\0';
	}
#endif
#if !RENDER_DL_GOLDEN
	/* Intent lamps, from fields the mailbox ALREADY carries: air_ticks/
	 * duck_ticks are the literal jump/duck state; a lane STEP is edge-
	 * detected against the previous frame's lane and held lit a few frames
	 * so a one-tick transition is readable. Once a frame, here, so the hold
	 * counts frames, not bands. */
	static int8_t  prev_lane = -1;
	static uint8_t left_hold, right_hold;

	if (prev_lane >= 0) {
		if (scene_in.lane < (uint8_t)prev_lane) {
			left_hold = LAMP_HOLD_TICKS;
		} else if (scene_in.lane > (uint8_t)prev_lane) {
			right_hold = LAMP_HOLD_TICKS;
		}
	}
	prev_lane   = (int8_t)scene_in.lane;
	vid_lamp[0] = left_hold > 0u;
	vid_lamp[1] = right_hold > 0u;
	vid_lamp[2] = scene_in.air_ticks > 0u;
	vid_lamp[3] = scene_in.duck_ticks > 0u;
	left_hold   = left_hold > 0u ? left_hold - 1u : 0u;
	right_hold  = right_hold > 0u ? right_hold - 1u : 0u;
#endif
}

/* The lamp captions: what the player DOES to make it light (the lane lamps
 * follow the lane step, the jump lamp the jump -- game/step.c). */
static const char *const lamp_cap[4] = { "LEFT ARM", "RIGHT ARM", "BOTH ARMS", "DUCK" };

/* Portrait: four lamps stacked down the left strip (a LAMP_SQ square over
 * its caption, one LAMP_CELL_H cell each), the label down the right. */
static void draw_strips_portrait(const vcv_t *cv)
{
	const int lcx = VID_STRIP_W / 2, rcx = TR_VID_W - VID_STRIP_W / 2;

	for (int i = 0; i < 4; i++) {
		int y = TR_VID_Y0 + i * LAMP_CELL_H + 16;

		cv_rect(cv,
		        lcx - LAMP_SQ / 2,
		        y,
		        LAMP_SQ,
		        LAMP_SQ,
		        vid_lamp[i] ? COLOR_LAMP_ON : COLOR_LAMP_OFF);
		video_text_c(cv,
		             lcx,
		             y + LAMP_SQ + 12,
		             lamp_cap[i],
		             LAMP_CAP_SCALE,
		             vid_lamp[i] ? COLOR_LAMP_ON : COLOR_LABEL);
	}
	video_text_c(cv, rcx, TR_VID_Y0 + 32, "CAMERA", 4, COLOR_LABEL);
	video_text_c(cv, rcx, TR_VID_Y0 + 64, vid_dims, 3, COLOR_LABEL);
	video_text_c(cv, rcx, TR_VID_Y0 + 128, "NPU", 4, COLOR_LABEL);
	video_text_c(cv, rcx, TR_VID_Y0 + 160, vid_hz, 5, vid_have_hz ? COLOR_KP : COLOR_LABEL);
}

/* Landscape: the four lamps side by side in the top letterbox band (a
 * LAND_LAMP_SQ square over its caption, one TR_VID_W / 4 cell each), the
 * "CAMERA / NPU Hz" label in the bottom band, camera on the left half and
 * NPU on the right. */
static void draw_strips_landscape(const vcv_t *cv)
{
	const int cell = TR_VID_W / 4, ly = TR_VID_Y0 + 8;
	const int by = TR_VID_Y0 + TR_VID_H - LAND_BAND_H + 12;

	for (int i = 0; i < 4; i++) {
		int cx = i * cell + cell / 2;

		cv_rect(cv,
		        cx - LAND_LAMP_SQ / 2,
		        ly,
		        LAND_LAMP_SQ,
		        LAND_LAMP_SQ,
		        vid_lamp[i] ? COLOR_LAMP_ON : COLOR_LAMP_OFF);
		video_text_c(cv,
		             cx,
		             ly + LAND_LAMP_SQ + 10,
		             lamp_cap[i],
		             LAMP_CAP_SCALE,
		             vid_lamp[i] ? COLOR_LAMP_ON : COLOR_LABEL);
	}
	video_text_c(cv, TR_VID_W / 4, by, "CAMERA", 4, COLOR_LABEL);
	video_text_c(cv, TR_VID_W / 4, by + 32, vid_dims, 3, COLOR_LABEL);
	video_text_c(cv, TR_VID_W * 3 / 4, by, "NPU", 4, COLOR_LABEL);
	video_text_c(cv, TR_VID_W * 3 / 4, by + 32, vid_hz, 5, vid_have_hz ? COLOR_KP : COLOR_LABEL);
}

/* The lamps and label around the picture, clipped to the canvas (one band's
 * rows), in the layout the camera view's rotation picks. */
static void draw_strips(const vcv_t *cv)
{
	if (vid_cv.rotate == 0u) {
		draw_strips_landscape(cv);
	} else {
		draw_strips_portrait(cv);
	}
}

/* Band rows [y_lo, y_lo + rows) -> the framebuffer, write-only, 16 B at a
 * time. One switch on the frame's rotation per band: 0 is the plain row copy,
 * 90 / 270 the NEON transpose (whole bands) with the rotation as a literal so
 * the mapping folds away; a ragged band (rows not a multiple of 8) takes the
 * scalar blit. `rows` <= TR_BAND_H, so a ragged last video band never
 * writes past TR_R3D_H. */
static inline __attribute__((always_inline)) void
copy_rows_turned(int rot, uint16_t *fb, const uint16_t *cband, int y_lo, int rows)
{
#if RENDER_A32 && (defined(__ARM_NEON) || defined(__ARM_NEON__))
	if (rows % 8 == 0 && y_lo % 8 == 0) {
		tr_rot_blit_neon(rot, fb, TR_R3D_H, cband, TR_R3D_W, 0, y_lo, TR_R3D_W, rows);
		return;
	}
#endif
	tr_rot_blit(rot, fb, TR_R3D_H, cband, TR_R3D_W, 0, y_lo, TR_R3D_W, rows);
}

static void copy_rows(uint16_t *fb, const uint16_t *cband, int y_lo, int rows)
{
	switch (frame_rot) {
	case 90:
		copy_rows_turned(90, fb, cband, y_lo, rows);
		break;
	case 270:
		copy_rows_turned(270, fb, cband, y_lo, rows);
		break;
	default: {
		uint16_t *d = &fb[(uint32_t)y_lo * TR_R3D_W];
		uint32_t  n = (uint32_t)rows * TR_R3D_W;

		for (uint32_t x = 0; x < n; x += 8) {
			copy16(&d[x], &cband[x]);
		}
	}
	}
}

static void fill_px(uint16_t *d, int n, uint16_t c)
{
	for (int i = 0; i < n; i++) {
		d[i] = c;
	}
}

/* Video band vb: video-area rows [vb * TR_BAND_H, +TR_BAND_H). The camera
 * rows are ONE column strip of the raw frame when rotated (an upright row
 * is a raw column), invalidated raw row by raw row just before the read. */
void render_video_band(uint32_t core, int vb, uint16_t *fb)
{
	uint32_t  t0    = ticks();
	uint16_t *cband = CBAND(core);
	int       ly0   = vb * TR_BAND_H;
	int       rows  = ly0 + TR_BAND_H <= TR_VID_H ? TR_BAND_H : TR_VID_H - ly0;

#if TR_CAM_PIP_ENABLE
	const vcv_t cv  = { cband, 0, TR_R3D_W, TR_VID_Y0 + ly0, TR_VID_Y0 + ly0 + rows, -1 };
	int         rot = vid_cv.rotate, ix0 = tr_cam_img_x0(rot), uw = TR_CAM_UP_W(rot);
	int         top = tr_cam_img_y0(rot), iy0 = 0, iy1 = 0; /* this band's image rows, area-local */

	if (vid_have_cv) {
		iy0 = ly0 > top ? ly0 : top;
		iy1 = ly0 + rows < top + TR_CAM_UP_H(rot) ? ly0 + rows : top + TR_CAM_UP_H(rot);
	}
	for (int r = 0; r < rows; r++) {
		uint16_t *d = cband + (uint32_t)r * TR_R3D_W;

		if (ly0 + r >= iy0 && ly0 + r < iy1) {
			fill_px(d, ix0, COLOR_PANEL_BG); /* the strips only: the image covers the middle */
			fill_px(d + ix0 + uw, TR_R3D_W - ix0 - uw, COLOR_PANEL_BG);
		} else {
			fill_px(d, TR_R3D_W, COLOR_PANEL_BG);
		}
	}
	if (iy1 > iy0) {
		const uint8_t *buf = (const uint8_t *)(uintptr_t)vid_cv.buf_addr;
		uint16_t      *dst = cband + (uint32_t)(iy0 - ly0) * TR_R3D_W + (uint32_t)ix0;
		int            uy0 = iy0 - top, n = iy1 - iy0;

		if (rot == 0) {
			/* Landscape: raw rows 1:1, scalar. */
			dcache_inval_range(buf + (uint32_t)uy0 * TR_CAM_SRC_W, (uint32_t)n * TR_CAM_SRC_W);
			for (int r = 0; r < n; r++) {
				tr_cam_pip_row_grey_to_rgb565(buf + (uint32_t)(uy0 + r) * TR_CAM_SRC_W,
				                              TR_CAM_SRC_W,
				                              dst + (uint32_t)r * TR_R3D_W,
				                              TR_CAM_SRC_W);
			}
		} else {
			int c0, c1;

			tr_cam_rot_src_cols(rot, uy0, n, &c0, &c1);
			for (int s = 0; s < TR_CAM_SRC_H; s++) {
				dcache_inval_range(buf + (uint32_t)s * TR_CAM_SRC_W + (uint32_t)c0,
				                   (uint32_t)(c1 - c0));
			}
#if RENDER_A32 && (defined(__ARM_NEON) || defined(__ARM_NEON__))
			if (uy0 % 8 == 0 && n % 8 == 0) {
				tr_cam_rot_rows_neon(buf, rot, uy0, n, dst, TR_R3D_W);
			} else
#endif
			{
				tr_cam_rot_rows(buf, TR_CAM_SRC_W, TR_CAM_SRC_H, rot, uy0, n, dst, TR_R3D_W);
			}
		}
	}
	draw_strips(&cv);
	copy_rows(fb, cband, TR_VID_Y0 + ly0, rows);
#else
	(void)cband;
	(void)rows;
	(void)fb;
#endif
	render_core_stats[core].video_ticks += ticks() - t0;
}

/* The skeleton over the camera -- a few hundred px, core 0, once, after
 * every band (3D and video) has landed; straight into the framebuffer,
 * clipped to the image. Reads this frame's own vid_cv, so it matches the
 * image the bands drew. */
void render_video_overlay(uint16_t *fb)
{
	uint32_t t0 = ticks();

#if TR_CAM_PIP_ENABLE
	tr_pslot_t out;
	uint32_t   seq;

	if (vid_have_cv && tr_pslot_read(PIP_PSLOT_ADDR, 0u, &out, &seq, pip_barrier) &&
	    out.hp_state == TR_HP_STATE_RUNNING) {
		int rot = vid_cv.rotate, x0 = tr_cam_img_x0(rot), y0 = TR_VID_Y0 + tr_cam_img_y0(rot);
		const vcv_t cv = { fb, x0, x0 + TR_CAM_UP_W(rot), y0, y0 + TR_CAM_UP_H(rot), frame_rot };
		int16_t     px[TR_POSE_KP], py[TR_POSE_KP];
		bool        ok[TR_POSE_KP];

		for (int k = 0; k < TR_POSE_KP; k++) {
			ok[k] = tr_cam_pip_map_kp(&out.pose.kp[k], rot, &px[k], &py[k]);
		}
		for (unsigned b = 0; b < 12u; b++) {
			int a = bones[b][0], z = bones[b][1];

			if (ok[a] && ok[z]) {
				video_line(&cv, px[a], TR_VID_Y0 + py[a], px[z], TR_VID_Y0 + py[z], COLOR_KP);
			}
		}
		for (int k = 0; k < TR_POSE_KP; k++) {
			if (ok[k]) {
				video_dot(&cv, px[k], TR_VID_Y0 + py[k], COLOR_KP);
			}
		}
	}
#else
	(void)fb;
#endif
	render_video_panel_ticks = ticks() - t0;
}

uint32_t render_front(const tr_frame_in_t *in)
{
	uint32_t t0 = ticks();

#if !RENDER_DL_GOLDEN
	/* fix round 9: the split path (render_front_begin) sets scene_in
	 * itself before build_dl's own tr_scene_step call; this single-call
	 * path drove build_dl the same way but left scene_in stale (zero on
	 * the first frame, whatever the last split-path caller left it on
	 * after), so render_video_overlay's lamps (scene_in.lane/air_ticks/
	 * duck_ticks) silently never lit for a caller using render_front/
	 * render_setup, only for the real renderer.c main loop's own
	 * begin/part/end split sequence. Found via the dump tool's own
	 * lamp-lit render coming out byte-identical to the lamp-off one. */
	if (in != NULL) {
		scene_in = *in;
	}
#endif
	build_dl(in, DL, &frame_bg);
	hud_score  = in != NULL ? in->score : 0u;
	hud_banner = in != NULL ? in->banner : 0u;
	hud_flags  = in != NULL ? in->flags : 0u;
	frame_rot  = in != NULL && !RENDER_DL_GOLDEN ? in->rotation : 0;
	video_frame_state();
	render_stats = (render_stats_t){ ticks() - t0, 0, DL->n, 0, tr_dl_dropped, 0 };
	return DL->n;
}

void render_front_begin(const tr_frame_in_t *in)
{
	front_t0   = ticks();
	hud_score  = in != NULL ? in->score : 0u;
	hud_banner = in != NULL ? in->banner : 0u;
	hud_flags  = in != NULL ? in->flags : 0u;
	frame_rot  = in != NULL && !RENDER_DL_GOLDEN ? in->rotation : 0;
#if RENDER_DL_GOLDEN
	build_dl(in, DL, &frame_bg);
#else
	scene_in = *in;
	tr_scene_step(&scene, &scene_in);
	tr_dl_dropped = 0;
#endif
}

void render_set_quality(uint8_t q)
{
#if RENDER_DL_GOLDEN
	(void)q;
#else
	scene.quality = q;
#endif
}

void render_front_part(int part)
{
#if RENDER_DL_GOLDEN
	(void)part;
#else
	tr_cam_t cam;

	if (part == 0) {
		tr_dl_dropped = 0;
	}
	tr_scene_build_part(
	    &scene, &scene_in, part == 2 ? &cam : &scene_cam, part == 2 ? DL1 : DL, part);
#endif
}

uint32_t render_front_end(int part2)
{
#if !RENDER_DL_GOLDEN
	if (part2) {
		uint32_t room = TR_DL_MAX_TRIS - DL->n, k = DL1->n < room ? DL1->n : room;

		memcpy(&DL->tri[DL->n], DL1->tri, k * sizeof(DL->tri[0]));
		DL->n = (uint16_t)(DL->n + k);
		tr_dl_dropped += DL1->n - k;
	}
	tr_scene_bg(&scene_in, &scene_cam, &frame_bg);
	tr_scene_bg_flash(&scene_in, &frame_bg);
#else
	(void)part2;
#endif
	video_frame_state();
	render_stats = (render_stats_t){ ticks() - front_t0, 0, DL->n, 0, tr_dl_dropped, 0 };
	return DL->n;
}

void render_setup_part(uint32_t lo, uint32_t hi)
{
	tr_tri_setup_range(DL, SETUP, lo, hi);
}

void render_bin(void)
{
	uint32_t overflow = 0, max_bin = 0;

	tr_bin_only(DL, SETUP, BINS, counts, &overflow);
	for (int b = 0; b < TR_BANDS; b++) {
		max_bin = counts[b] > max_bin ? counts[b] : max_bin;
	}
	render_stats.dropped = overflow;
	render_stats.max_bin = max_bin;
}

void render_setup(const tr_frame_in_t *in)
{
	uint32_t n  = render_front(in);
	uint32_t t0 = ticks();

	render_setup_part(0, n);
	render_bin();
	render_stats.bin = ticks() - t0;
}

uint32_t render_video_panel_ticks; /* fix round 8 item 3, still true after round 10's split: the
				     * overlay's (skeleton/lamps/label, render_video_overlay())
				     * own cost, published by the caller (renderer.c) into the
				     * bench-readable stats block -- a plain global, not
				     * render_core_stats (which stays in D-cache and reads 0 to
				     * an external raw SRAM read unless cleaned; this is simpler
				     * to just publish through the SAME word-array mechanism the
				     * caller already proved reachable). The camera bands'
				     * own cost is render_core_stats[core].video_ticks instead
				     * (render.h): unlike this overlay they run on EITHER core,
				     * so a single global would race. */

void render_band(uint32_t core, int b, uint16_t *fb)
{
	render_core_stats_t *st = &render_core_stats[core];
	uint32_t             ta = ticks();
	/* Raster only the game viewport's rows: a ragged last band (a TR_VIEW_H
	 * that is not TR_BAND_H-aligned) is clipped here, and its copy's extra
	 * rows are overwritten by video band 0 before the frame publishes. */
	int y_hi = (b + 1) * TR_BAND_H < TR_VIEW_H ? (b + 1) * TR_BAND_H : TR_VIEW_H;

	tr_raster_band(NULL,
	               0,
	               b * TR_BAND_H,
	               y_hi,
	               ZBAND(core),
	               CBAND(core),
	               &frame_bg,
	               DL,
	               SETUP,
	               BINS[b],
	               counts[b]);
	/* TR_FLAG_HUD_L2: the HE shows score + banners on CDC200 layer 2 (P9),
	 * so the bands carry the 3D picture only. */
	if (render_hud && !(hud_flags & TR_FLAG_HUD_L2)) {
		hud_band(CBAND(core), b * TR_BAND_H);
	}
	uint32_t tb = ticks();

	copy_rows(fb, CBAND(core), b * TR_BAND_H, TR_BAND_H);
	st->raster += tb - ta;
	st->copy += ticks() - tb;
	st->bands++;
}

/* Single-core convenience wrapper (tests, tools, render_frame() below): every
 * video band on core 0 in order, then the overlay -- same total effect as
 * the old single-threaded draw_video_panel(), now through the cached+NEON
 * band path. The real dual-core renderer (renderer.c) does NOT call this:
 * it claims render_video_band() through band_loop() (either core, self-
 * balancing with the 3D bands) and calls render_video_overlay() itself,
 * once, from core 0, after its join_core1() -- see that file for why. */
void render_video_panel(uint16_t *fb)
{
	for (int vb = 0; vb < TR_VIDEO_BANDS; vb++) {
		render_video_band(0, vb, fb);
	}
	render_video_overlay(fb);
}

void render_frame(const tr_frame_in_t *in, uint16_t *fb)
{
	render_setup(in);
	for (int b = 0; b < TR_BANDS; b++) {
		render_band(0, b, fb);
	}
	render_video_panel(fb);
}

uint32_t render_fb_crc(const uint16_t *fb)
{
	return tr_crc32(0, fb, RENDER_FB_BYTES);
}

uint32_t render_band_crc(const uint16_t *fb, int b)
{
	return tr_crc32(0, &fb[(uint32_t)b * BAND_PX], BAND_PX * 2u);
}
