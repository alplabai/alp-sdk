/* tests/host/test_a32_scene.c -- the A32 renderer's scene build
 * (a32/renderer/render.c, RENDER_DL_GOLDEN=0) through the dual-core band
 * path: render_setup + bands in a shuffled order, the two cores' band
 * buffers interleaved.
 *   1. Frame 1 is the scene golden (tr_scene_golden.h): its CRC must be
 *      TR_SCENE_GOLDEN_CRC, the same raster test_r3d_scene.c checks.
 *   2. Frames 2..5 (lane change, pickup burst, crash, crash + phase) must equal a reference
 *      render of an independent tr_scene_t fed the same inputs; frame 2 goes
 *      through the dual-core front end (scene parts 2 then 1, setup halves
 *      in reverse order).
 *   3. HUD on (score + banner): band order does not matter, and it touches
 *      only the top 64 rows -- unless TR_FLAG_HUD_L2 (the HE draws it). */
#include <assert.h>
#include <stdio.h>

#define RENDER_DL_GOLDEN 0
#include "../../a32/common/crc32.c"
#include "../../a32/renderer/render.c"
#include "tr_scene_golden.h"

#define W TR_R3D_W
#define H TR_R3D_H

static uint16_t       fb[W * H] __attribute__((aligned(16))), ref[W * H], hud[W * H];
static uint16_t       rz[W * TR_BAND_H], rc[W * TR_BAND_H];
static uint16_t       rbins[TR_BANDS][TR_BIN_MAX];
static uint32_t       rcounts[TR_BANDS];
static tr_tri_setup_t rsetup[TR_DL_MAX_TRIS];
static tr_dl_t        rdl;

/* test_r3d_scene.c's reference: bands in order straight into the frame. */
static void ref_frame(tr_scene_t *s, const tr_frame_in_t *in)
{
	tr_cam_t cam;
	tr_bg_t  bg;
	uint32_t overflow = 0;

	tr_scene_step(s, in);
	tr_scene_build(s, in, &cam, &rdl);
	tr_scene_bg(in, &cam, &bg);
	tr_scene_bg_flash(in, &bg);
	tr_bin_build(&rdl, rsetup, rbins, rcounts, &overflow);
	for (int b = 0; b < TR_BANDS; b++) {
		/* fix round 8: clip the last band to TR_VIEW_H, matching
		 * render_band()'s own clip (r3d.h's TR_VIEW_H comment) -- this
		 * reference must agree with what render_band() actually draws. */
		int y_hi = (b + 1) * TR_BAND_H < TR_VIEW_H ? (b + 1) * TR_BAND_H : TR_VIEW_H;

		tr_raster_band(
		    ref, W, b * TR_BAND_H, y_hi, rz, rc, &bg, &rdl, rsetup, rbins[b], rcounts[b]);
	}
}

/* fix round 8: see test_a32_render.c's identically-named helper -- the
 * ragged last band's copy_band() moves a full 32-row cband slab, so fb's
 * rows [TR_VIEW_H, H) carry a stale, reused-buffer artifact this test's
 * own poison (memset 0xA5+f) does not reproduce in `ref`. Neither side's
 * content there is meaningful (render_video_panel(), not called by this
 * test, is the real owner of those rows) -- zero both before comparing. */
static void clear_below_view(uint16_t *f)
{
	memset(&f[TR_VIEW_H * W], 0, (size_t)(H - TR_VIEW_H) * W * sizeof(uint16_t));
}

static uint32_t rs = 0x2545F491u;

/* All bands in a fresh shuffled order, cores alternating. */
static void bands_shuffled(uint16_t *dst)
{
	int order[TR_BANDS];

	for (int b = 0; b < TR_BANDS; b++) {
		order[b] = b;
	}
	for (int i = TR_BANDS - 1; i > 0; i--) {
		rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
		int j = (int)(rs % (uint32_t)(i + 1)), t = order[i];

		order[i] = order[j], order[j] = t;
	}
	for (int i = 0; i < TR_BANDS; i++) {
		render_band((uint32_t)i & 1u, order[i], dst);
	}
}

int main(void)
{
	tr_scene_t    rscene;
	tr_frame_in_t in[5];

	in[0] = tr_scene_golden_in(1234, 1);
	in[1] = tr_scene_golden_in(1235, 0); /* lane change: bank + ease */
	in[2] = tr_scene_golden_in(1236, 0);
	in[2].score += 10; /* pickup: particle burst */
	/* P6: a crash (burst, flash border, red background) then a later
	 * crash frame at half a tick of attract phase, both dual-core. */
	in[3]            = in[2];
	in[3].flags      = TR_FLAG_CRASH;
	in[3].ents[4]    = (tr_pkt_ent_t){ 1, 0, 1, 0, 1110, 0 };
	in[3].crash_ent  = 4;
	in[3].crash_kind = TR_CRASH_KIND_LOW;
	in[4]            = in[3];
	in[4].crash_tick = 3;
	in[4].phase      = 32768;
	in[4].flags |= TR_FLAG_PHASE;

	render_init();
	render_hud = 0;
	tr_scene_init(&rscene);
	for (int f = 0; f < 5; f++) {
		memset(fb, 0xA5 + f, sizeof(fb));
		if (f == 1 || f >= 3) { /* the dual-core path: scene in two parts, setup in two halves */
			render_front_begin(&in[f]);
			render_front_part(2); /* core 1 may finish first */
			render_front_part(1);

			uint32_t n = render_front_end(1);

			render_setup_part(n / 2u, n);
			render_setup_part(0, n / 2u);
			render_bin();
		} else if (f == 0) { /* single-core front end (a core-1 fallback) */
			render_front_begin(&in[f]);
			render_front_part(0);

			uint32_t n = render_front_end(0);

			render_setup_part(0, n);
			render_bin();
		} else {
			render_setup(&in[f]);
		}
		assert(render_stats.dropped == 0 && render_stats.dl_dropped == 0);
		assert(render_stats.max_bin > 0 && render_stats.max_bin <= TR_BIN_MAX);
		bands_shuffled(fb);
		ref_frame(&rscene, &in[f]);
		clear_below_view(fb);
		clear_below_view(ref);

		uint32_t crc = render_fb_crc(fb);

		printf("a32 scene frame %d: %u tris, max bin %u, crc %08x\n",
		       f,
		       (unsigned)render_stats.tris,
		       (unsigned)render_stats.max_bin,
		       (unsigned)crc);
		assert(crc == tr_crc32(0, ref, sizeof(ref)));
		if (f == 0) {
			assert(crc == TR_SCENE_GOLDEN_CRC);
		}
	}

	/* HUD: same DL (no new setup), banner + score on. */
	hud_banner = TR_BANNER_ATTRACT;
	hud_score  = 4321;
	render_hud = 1;
	for (int b = 0; b < TR_BANDS; b++) {
		render_band(0, b, hud);
	}
	bands_shuffled(fb);
	clear_below_view(fb);
	clear_below_view(hud);
	assert(memcmp(fb, hud, sizeof(fb)) == 0);
	assert(memcmp(&hud[64 * W], ref + 64 * W, sizeof(hud) - 64 * W * sizeof(hud[0])) == 0);
	assert(memcmp(hud, ref, 64 * W * sizeof(hud[0])) != 0);
	/* P9: with TR_FLAG_HUD_L2 the HE draws the HUD on CDC200 layer 2 --
	 * the same HUD-on render leaves the bands exactly the 3D reference. */
	hud_flags = TR_FLAG_HUD_L2;
	for (int b = 0; b < TR_BANDS; b++) {
		render_band(0, b, hud);
	}
	clear_below_view(hud);
	assert(memcmp(hud, ref, sizeof(hud)) == 0);
	printf("a32 scene: golden crc %08x via bands, 5 frames == reference, HUD order-independent\n",
	       (unsigned)TR_SCENE_GOLDEN_CRC);
	return 0;
}
