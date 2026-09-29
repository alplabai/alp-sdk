/* a32/renderer/render.h -- one frame into a framebuffer: DL -> tr_bin_build ->
 * 40 bands x 32 rows (tr_raster_band into the cached colour/z band, HUD +
 * banner blitted into the band) -> write-only row copy to the FB.
 * DL source, build define RENDER_DL_GOLDEN: 0 (default) the PCB-canyon scene
 * (src/render/r3d_scene.h: tr_scene_step + tr_scene_build + tr_scene_bg on
 * the mailbox snapshot); 1 the committed golden DL (tests/host/
 * tr_golden_dl.h, CP-A6 regression: no HUD, per-band golden CRCs). Portable C: the A32 renderer image links it
 * with its buffers at the fixed SRAM1 addresses of the a32-renderer plan
 * (sec 4); the host test (tests/host/test_a32_render.c) links it with static
 * buffers and checks the golden raster CRC.
 */
#ifndef TR_A32_RENDER_H
#define TR_A32_RENDER_H

#include <stdint.h>

#include "../../src/ipc/tr_mbox.h"
#include "../../src/render/cam_pip.h" /* TR_VID_H, the video area's own row count */
#include "../../src/render/r3d.h"     /* TR_BAND_H, the same band height the 3D raster uses */

#ifndef RENDER_DL_GOLDEN
#define RENDER_DL_GOLDEN 0
#endif

#define RENDER_FB_BYTES (720u * 1280u * 2u)

/* Two cores render bands; the band buffers (z + colour) are per core. */
#define RENDER_CORES 2

/* Per-frame stats of the last render_setup(), CNTVCT ticks (0 on host). */
typedef struct {
	uint32_t scene;      /* DL build: scene step + build + bg (golden: DL copy) */
	uint32_t bin;        /* triangle setup + binning (core 0's view: its half + wait + bin) */
	uint32_t tris;       /* DL triangles */
	uint32_t dropped;    /* (triangle, band) pairs lost to a full bin */
	uint32_t dl_dropped; /* triangles the front-end dropped (tr_dl_dropped; golden 0) */
	uint32_t max_bin;    /* fullest band bin (of TR_BIN_MAX) */
} render_stats_t;
extern render_stats_t render_stats;

/* Per-core band stats, accumulated by render_band() until the caller zeroes
 * them (one cache line each: the cores never share one). */
typedef struct {
	uint32_t raster;      /* tr_raster_band: background, z clear, triangles */
	uint32_t copy;        /* band -> FB row copy */
	uint32_t bands;       /* bands rendered */
	uint32_t video_ticks; /* fix round 10: render_video_band() calls this core did, this frame */
	uint32_t pad[12];
} render_core_stats_t;
extern render_core_stats_t render_core_stats[RENDER_CORES];

/* The video area (cam_pip.h TR_VID_H rows) is claimable band work, the same
 * TR_BAND_H granularity and band_claim/band_loop mechanism (renderer.c) the
 * 3D bands use (fix round 10: one single-threaded uncached pass cost 35.9
 * ms/frame) -- whichever core is free claims the next band. A ragged last
 * video band is clipped (render_video_band()/copy_video_rows()), never
 * writing past TR_R3D_H. */
#define TR_VIDEO_BANDS ((TR_VID_H + TR_BAND_H - 1) / TR_BAND_H)

/* Once per entry: textures, scene state. */
void render_init(void);

/* Draw the score + banner HUD into the bands (scene build default 1, golden
 * build 0; the host test turns it off to compare with the scene golden). */
extern int render_hud;

/* One frame = render_setup(in) once (core 0), then render_band(core, b, fb)
 * for every b in 0..TR_BANDS-1, in any order, from either core -- each band
 * is independent (own rows of `fb`, the core's own z/colour band). `fb` is
 * 720x1280 RGB565, stride 720, 16-byte aligned. render_setup() advances the
 * scene state by one frame (scene build) and keeps in->score/banner for the
 * HUD; `in` is unused by the golden build. */
void render_setup(const tr_frame_in_t *in);
void render_band(uint32_t core, int b, uint16_t *fb);

/* render_setup() in three steps, so two cores can split the triangle
 * setup: render_front(in) builds the DL (returns its triangle count n) and
 * sets stats scene/tris/dl_dropped; render_setup_part(lo, hi) builds the
 * setup records [lo, hi) -- any split, either core, same bits; render_bin()
 * bins them in DL order once all n are done (stats dropped/max_bin). The
 * caller times the setup + bin into render_stats.bin. */
uint32_t render_front(const tr_frame_in_t *in);

/* render_front() split for two cores (scene build only; the golden build
 * does all its work in _begin): core 0 render_front_begin(in) (scene step,
 * input copy), then core 0 render_front_part(1) and core 1
 * render_front_part(2) concurrently (tr_scene_build_part(); part 2 into its
 * own DL), then core 0 render_front_end(1) appends part 2's DL (the same DL
 * as render_front()), sets the background and stats, returns n. One core:
 * render_front_part(0) (the whole scene into core 0's DL, never touching
 * part 2's) then render_front_end(0). */
void     render_front_begin(const tr_frame_in_t *in);
/* Scene detail for the next frames: tr_scene_t.quality (TR_LOD_* bits,
 * r3d_scene.h); 0 = full, the golden image. Core 0, between frames. */
void     render_set_quality(uint8_t q);
void     render_front_part(int part);
uint32_t render_front_end(int part2);
void     render_setup_part(uint32_t lo, uint32_t hi);
void     render_bin(void);

/* Single core: render_setup + all bands in order on core 0. */
void render_frame(const tr_frame_in_t *in, uint16_t *fb);

/* fix round 8 item 3: draw_video_panel()'s own cost, this frame, CNTVCT
 * ticks (100 MHz -- /100 for us). Written by render_video_panel(); the real
 * dual-core renderer (renderer.c) publishes it into the bench-readable
 * stats block after calling render_video_panel() once, from core 0, after
 * every band is drawn (join_core1()). */
extern uint32_t render_video_panel_ticks;
void            render_video_panel(uint16_t *fb);

/* render_video_band(core, vb, fb) draws video band vb (0..TR_VIDEO_BANDS-1)
 * -- background, the upright camera rows (NEON 8x8 transpose when rotated),
 * lamps, label -- into the core's own cached scratch, invalidating just the
 * raw bytes it reads, then a NEON copy out: render_core_stats[core].
 * video_ticks. render_video_overlay(fb): the skeleton only, a few hundred
 * px, once by core 0 after every band -- render_video_panel_ticks. Both read
 * the frame state render_front()/render_front_end() took once (camera view,
 * lamps, label). render_video_panel(fb) (above) is the single-core wrapper
 * (tests, tools, render_frame): every video band on core 0, then the
 * overlay. */
void render_video_band(uint32_t core, int vb, uint16_t *fb);
void render_video_overlay(uint16_t *fb);

/* TR_GOLDEN_RASTER_CRC / TR_GOLDEN_BAND_CRC (tests/host/tr_golden_dl.h,
 * generated): what render_fb_crc() / render_band_crc() give for the golden DL. */
extern const uint32_t render_golden_crc;
extern const uint32_t render_golden_band_crc[];

/* CRC-32 (zlib) of `fb`: the whole frame, or band b's 32 rows. */
uint32_t render_fb_crc(const uint16_t *fb);
uint32_t render_band_crc(const uint16_t *fb, int b);

#endif /* TR_A32_RENDER_H */
