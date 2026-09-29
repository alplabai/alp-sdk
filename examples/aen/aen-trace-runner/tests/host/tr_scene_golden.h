/* tests/host/tr_scene_golden.h -- the scene golden frame, shared by
 * test_r3d_scene.c (reference raster) and test_a32_scene.c (the A32
 * renderer's band path): tr_scene_init, one tr_scene_step and
 * tr_scene_build on tr_scene_golden_in(1234, 1), rasterised, give
 * TR_SCENE_GOLDEN_CRC.
 */
#ifndef TR_SCENE_GOLDEN_H
#define TR_SCENE_GOLDEN_H

#include "../../src/ipc/tr_mbox.h"

/* CRC-32 of the golden frame (host glibc == A32 newlib, -ffp-contract=off).
 * Update only on a deliberate scene/mesh/palette change. fix round 9:
 * updated for the camera's own pitch retune (r3d_scene.h TR_CAM_PITCH_DEG,
 * 18 -> 23 deg) -- run test_r3d_scene.c and paste its own printed "golden
 * scene crc32". Half/half layout: updated for TR_VIEW_H 853 -> 640 (r3d.h;
 * the focal length and cy follow it), same procedure. */
#define TR_SCENE_GOLDEN_CRC 0xe65d8d5eu

/* World zones (P15, test_r3d_zones.c): tr_scene_golden_in(1234, 1) in each
 * zone (TR_FLAG_ZONE, no gate), and in the die city with its gate at model
 * y 1080, phase 0.5: mid-way through the blend into the memory canyon. */
/* fix round 9: updated for the camera's own pitch retune -- run
 * test_r3d_zones.c with -DTR_ZONES_PRINT_ONLY and paste its own printed
 * "zones: golden ..." lines. */
/* Half/half layout (TR_VIEW_H 640): same procedure. */
#define TR_ZONE_GOLDEN_CRC \
	{ TR_SCENE_GOLDEN_CRC, 0x8d592172u, 0x039a528cu, 0x17c67be9u, 0x80a3686bu }
#define TR_ZONE_BLEND_GOLDEN_CRC 0xd87be5d7u

/* The skinned run cycle at 16 phases, every character (test_r3d_scene case 0), same rule. */
#define TR_RIG_RUN_CRC 0x4f3cfa95u

/* The reference frame: mid-run, centre lane, a mix of both obstacle kinds
 * and pickups spread from spawn to just past the runner. */
static inline tr_frame_in_t tr_scene_golden_in(uint32_t tick, uint8_t lane)
{
	static const struct {
		uint8_t kind, lane, low;
		int16_t y;
	} e[] = {
		{ 1, 0, 1, 40 },  { 2, 1, 0, 150 }, { 1, 2, 0, 300 }, { 2, 0, 0, 420 },  { 1, 1, 1, 560 },
		{ 1, 2, 1, 700 }, { 2, 2, 0, 820 }, { 1, 0, 0, 930 }, { 2, 1, 0, 1010 }, { 1, 2, 0, 1080 },
	};
	tr_frame_in_t in = { 0 };

	in.tick  = tick;
	in.score = 120;
	in.flags = TR_FLAG_ALIVE;
	in.lane  = lane;
	for (unsigned i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
		in.ents[i] = (tr_pkt_ent_t){ e[i].kind, e[i].lane, e[i].low, 0, e[i].y, 0 };
	}
	return in;
}

#endif /* TR_SCENE_GOLDEN_H */
