/* tests/host/test_a32_render.c -- the A32 renderer's render_frame() over the
 * golden DL (a32/renderer/render.c) must give TR_GOLDEN_RASTER_CRC and every
 * TR_GOLDEN_BAND_CRC. The dual-core schedule (bands claimed in any order,
 * alternating the two cores' band buffers) must give the same frame. */
#include <assert.h>
#include <stdio.h>

#define RENDER_DL_GOLDEN 1 /* the CP-A6 golden build; the scene build is test_a32_scene.c */
#include "../../a32/common/crc32.c"
#include "../../a32/renderer/render.c"

static uint16_t fb[TR_R3D_W * 1280] __attribute__((aligned(16)));

/* render_band() is contractually responsible for rows [0, TR_VIEW_H) only
 * (a ragged last band, if TR_VIEW_H is ever not TR_BAND_H-aligned, copies a
 * few rows past it that video band 0 overwrites in the real pipeline).
 * Zeroing [TR_VIEW_H, H) matches what draw_banded() (test_r3d_band.c)
 * produces. */
static void clear_below_view(uint16_t *f)
{
	memset(&f[TR_VIEW_H * TR_R3D_W], 0, (size_t)(1280 - TR_VIEW_H) * TR_R3D_W * sizeof(uint16_t));
}

int main(void)
{
	render_init();

	/* 1. plain frame CRC, per-band CRCs -- setup + bands ONLY (not
	 * render_frame(), which since fix round 8 also draws the bottom video
	 * panel: render_video_panel() overwrites rows [TR_VIEW_H, TR_R3D_H)
	 * with the (live-camera-dependent, not a committed-golden concern)
	 * panel background. TR_GOLDEN_RASTER_CRC covers the full sizeof(fb)
	 * with that region zeroed -- draw_banded() (test_r3d_band.c) clips its
	 * own last band to TR_VIEW_H the same way render_band() below does. */
	render_setup(NULL);
	for (int b = 0; b < TR_BANDS; b++) {
		render_band(0, b, fb);
	}
	clear_below_view(fb);
	assert(render_stats.tris == TR_GOLDEN_DL_N && render_stats.dropped == 0);
	assert(render_fb_crc(fb) == render_golden_crc);
	for (int b = 0; b < TR_BANDS; b++) {
		assert(render_band_crc(fb, b) == render_golden_band_crc[b]);
	}

	/* 3. dual-core order independence: shuffled bands, cores alternating,
	 * from a differently poisoned FB and dirty band buffers. */
	int      order[TR_BANDS];
	uint32_t rs = 0x9E3779B9u;

	for (int b = 0; b < TR_BANDS; b++) {
		order[b] = b;
	}
	for (int round = 0; round < 3; round++) {
		for (int i = TR_BANDS - 1; i > 0; i--) { /* Fisher-Yates, xorshift */
			rs ^= rs << 13, rs ^= rs >> 17, rs ^= rs << 5;
			int j = (int)(rs % (uint32_t)(i + 1)), t = order[i];

			order[i] = order[j], order[j] = t;
		}
		memset(fb, 0x5A + round, sizeof(fb));
		memset(render_core_stats, 0, sizeof(render_core_stats));
		render_setup(NULL);
		for (int i = 0; i < TR_BANDS; i++) {
			render_band((uint32_t)(i + round) & 1u, order[i], fb);
		}
		assert(render_core_stats[0].bands + render_core_stats[1].bands == TR_BANDS);
		clear_below_view(fb);
		assert(render_fb_crc(fb) == render_golden_crc);
	}
	printf("a32 render: golden crc %08x ok, per band ok, band order free\n",
	       (unsigned)render_golden_crc);
	return 0;
}
