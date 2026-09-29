/* tests/host/test_r3d_math.c -- tr_cam_build()/tr_r3d_project()/
 * tr_r3d_emit_quad()/tr_r3d_emit_mesh(): known-point projection, near-plane
 * clip triangle counts, guard-band clamp, backface cull, display-list
 * overflow. See src/render/r3d.h for the contracts under test.
 */
#include <assert.h>
#include <stdio.h>
#include <math.h>
#include <stddef.h>

#include "../../src/render/r3d.h"

/* An identity camera: eye at the world origin, no yaw/pitch/roll, so
 * camera space == world space exactly -- every hand-worked expected value
 * below is computed straight from tr_r3d_project()'s own formula
 * (sx = cx + x*f/z, sy = cy - y*f/z) with no rotation/translation to also
 * get right, which is what makes this a test of tr_r3d_project() and not
 * also, silently, of tr_cam_build()'s rotation maths. */
static void identity_cam(tr_cam_t *c, float f_px)
{
	tr_cam_build(c, (tr_v3_t){0, 0, 0}, 0.0f, 0.0f, 0.0f, f_px);
}

int main(void)
{
	/* 1. Known-point projection, identity camera. */
	{
		tr_cam_t c;
		tr_sv_t  out;
		float    view_z;

		identity_cam(&c, 256.0f);
		/* fix round 8: cy is TR_VIEW_H/2 now (r3d_math.c tr_cam_build),
		 * not TR_R3D_H/2 -- see r3d.h's TR_VIEW_H comment. */
		assert(c.cx == (float)TR_R3D_W / 2.0f && c.cy == (float)TR_VIEW_H / 2.0f);

		/* Straight ahead at z == f_px: scale is 1x, lands dead on centre. */
		assert(tr_r3d_project(&c, (tr_v3_t){0, 0, 256}, &out, &view_z));
		assert(view_z == 256.0f);
		assert(out.x == 360 * (1 << TR_R3D_SUB));
		/* fix round 8: cy is TR_VIEW_H/2 now -- (TR_VIEW_H << SUB) / 2,
		 * not 640 * (1 << SUB) (TR_R3D_H/2's old fixed-point value); exact
		 * since TR_VIEW_H << TR_R3D_SUB is always even. */
		assert(out.y == (TR_VIEW_H << TR_R3D_SUB) / 2);

		/* Off-centre point, same depth: scale 1x means world units and
		 * screen px coincide 1:1 (same anchor proj.c uses at its own
		 * Z_RUNNER, see proj.h). */
		assert(tr_r3d_project(&c, (tr_v3_t){100, 50, 256}, &out, &view_z));
		assert(out.x == 460 * (1 << TR_R3D_SUB));
		/* fix round 8: cy-centred, not 640 - 50 -- see the centre assert above. */
		assert(out.y == (TR_VIEW_H << TR_R3D_SUB) / 2 - 50 * (1 << TR_R3D_SUB)); /* y UP -> smaller screen y */

		/* Behind the near plane: view_z is still reported (a clip caller
		 * needs it), but the function reports not-visible and out is
		 * untouched. */
		view_z = -999.0f;
		assert(!tr_r3d_project(&c, (tr_v3_t){0, 0, 10}, &out, &view_z));
		assert(view_z == 10.0f);
		assert(!tr_r3d_project(&c, (tr_v3_t){0, 0, TR_CAM_Z_NEAR - 0.001f}, &out, &view_z));
		assert(tr_r3d_project(&c, (tr_v3_t){0, 0, TR_CAM_Z_NEAR}, &out, &view_z)); /* inclusive */
	}

	/* 2. Guard-band clamp: a point far enough off-axis (but at a visible
	 * depth) projects past +-TR_R3D_GUARD and must be clamped exactly to
	 * it, both directions -- not merely "reduced", exactly at the band. */
	{
		tr_cam_t c;
		tr_sv_t  out;
		float    view_z;

		identity_cam(&c, 256.0f);
		assert(tr_r3d_project(&c, (tr_v3_t){1.0e9f, 0, 40}, &out, &view_z));
		assert(out.x == TR_R3D_GUARD);
		assert(tr_r3d_project(&c, (tr_v3_t){-1.0e9f, 0, 40}, &out, &view_z));
		assert(out.x == -TR_R3D_GUARD);
		assert(tr_r3d_project(&c, (tr_v3_t){0, 1.0e9f, 40}, &out, &view_z));
		assert(out.y == -TR_R3D_GUARD); /* y up in world -> clamps to the small-y end */
		assert(tr_r3d_project(&c, (tr_v3_t){0, -1.0e9f, 40}, &out, &view_z));
		assert(out.y == TR_R3D_GUARD);
	}

	/*
	 * 3. Near-plane clip triangle counts, via tr_r3d_emit_quad(). Each
	 * case's SECOND triangle (q0,q2,q3) is made degenerate (q3 == q2) so
	 * it always contributes 0 regardless of clipping, isolating the first
	 * triangle's (q0,q1,q2) clip-count in the quad's total return value.
	 * Camera is the same identity camera as case 1, so world == camera
	 * space and TR_CAM_Z_NEAR applies directly to each point's z.
	 */
	{
		tr_cam_t c;
		tr_dl_t  dl = {0};

		identity_cam(&c, 256.0f);

		/*
		 * 2 vertices behind the near plane, 1 in front -> the clipped
		 * polygon is a triangle (3 verts) -> exactly 1 output triangle.
		 * Winding (q0,q1,q2) chosen front-facing (positive screen area)
		 * -- see this task's report for how that was pinned down: the
		 * mirror winding of these same 3 points is exactly the T4
		 * "backface cull" case below.
		 */
		{
			tr_v3_t q[4] = {
				{50, -50, 10}, /* behind */
				{-50, 0, 10}, /* behind */
				{0, 50, 200}, /* in front */
				{0, 50, 200}, /* == q[2]: 2nd tri of the quad is degenerate */
			};
			uint16_t n = tr_r3d_emit_quad(&dl, &c, q, 0x1234, 0);

			assert(n == 1);
			assert(dl.n == 1);
		}

		/* 1 vertex behind, 2 in front -> the clipped polygon is a quad
		 * (4 verts) -> exactly 2 output triangles. */
		{
			dl.n = 0;
			tr_v3_t q[4] = {
				{50, 0, 200}, /* in front */
				{-50, 0, 200}, /* in front */
				{0, 50, 10}, /* behind */
				{0, 50, 10}, /* == q[2]: 2nd tri of the quad is degenerate */
			};
			uint16_t n = tr_r3d_emit_quad(&dl, &c, q, 0x1234, 0);

			assert(n == 2);
			assert(dl.n == 2);
		}

		/* All 3 behind -> the whole triangle is clipped away: 0 output
		 * triangles, and this must NOT be counted in tr_dl_dropped (it's
		 * off-screen, not refused for lack of room). */
		{
			uint32_t before = tr_dl_dropped;

			dl.n = 0;
			tr_v3_t q[4] = {
				{-50, 0, 10}, {50, 0, 10}, {0, 50, 10}, {0, 50, 10},
			};
			uint16_t n = tr_r3d_emit_quad(&dl, &c, q, 0x1234, 0);

			assert(n == 0);
			assert(dl.n == 0);
			assert(tr_dl_dropped == before);
		}
	}

	/*
	 * 4. Backface cull: the SAME 4 world points, wound one way emit 2
	 * triangles (fully in front, no clip involved), wound the reverse way
	 * (swap q[1] and q[3], which reverses both triangles' winding) emit 0.
	 */
	{
		tr_cam_t c;
		tr_dl_t  dl = {0};

		identity_cam(&c, 256.0f);

		tr_v3_t q_front[4] = {
			{-40, -40, 200}, {-40, 40, 200}, {40, 40, 200}, {40, -40, 200},
		};
		uint16_t n_front = tr_r3d_emit_quad(&dl, &c, q_front, 0xABCD, 0);

		assert(n_front == 2);
		assert(dl.n == 2);

		dl.n = 0;
		tr_v3_t q_back[4] = {
			{-40, -40, 200}, {40, -40, 200}, {40, 40, 200}, {-40, 40, 200},
		};
		uint16_t n_back = tr_r3d_emit_quad(&dl, &c, q_back, 0xABCD, 0);

		assert(n_back == 0);
		assert(dl.n == 0);
	}

	/*
	 * 5. Display-list overflow: with dl.n already at TR_DL_MAX_TRIS,
	 * emitting a perfectly ordinary front-facing quad must append NOTHING
	 * (return 0, dl.n unchanged) and must count both of its would-be
	 * triangles in tr_dl_dropped -- not merely fail silently.
	 */
	{
		tr_cam_t c;
		tr_dl_t  dl = {0};

		identity_cam(&c, 256.0f);
		dl.n = TR_DL_MAX_TRIS;

		uint32_t before = tr_dl_dropped;
		tr_v3_t  q[4]   = {
			{-40, -40, 200}, {-40, 40, 200}, {40, 40, 200}, {40, -40, 200},
		};
		uint16_t n = tr_r3d_emit_quad(&dl, &c, q, 0xABCD, 0);

		assert(n == 0);
		assert(dl.n == TR_DL_MAX_TRIS);
		assert(tr_dl_dropped == before + 2);
	}

	/*
	 * 6. tr_r3d_emit_mesh() sanity: a single-triangle mesh, placed and
	 * yawed so it faces the identity camera dead-on, emits exactly 1
	 * triangle and resolves its palette colour -- proves the mesh path
	 * (vertex fetch, yaw rotation, normal rotation, palette lookup, then
	 * the same emit pipeline as emit_quad) wires together, not just the
	 * quad path already covered above.
	 */
	{
		static const int16_t v[3 * 3] = {
			-40, -40, 0, 40, -40, 0, 0, 40, 0,
		};
		static const uint8_t tri[3] = {0, 2, 1}; /* winding chosen front-facing, see test 4 */
		static const int8_t  nrm[3] = {0, 0, -127}; /* faces -z (toward eye at z=0) */
		static const uint8_t col[1] = {3}; /* palette[3] is non-black, see r3d_math.c */
		tr_mesh_t             mesh  = {v, tri, nrm, col, 3, 1, NULL, NULL, NULL, 0};
		tr_cam_t              c;
		tr_dl_t               dl = {0};
		tr_light_t             l = {{0, 0, -1}, 0.2f, {8, 8, 8}, 5000.0f, 9000.0f, NULL, 0.0f, 0.0f};

		identity_cam(&c, 256.0f);
		tr_inst_t in = {&mesh, {0, 0, 200}, 0.0f, 1.0f, 0, 0.0f};
		uint16_t  n  = tr_r3d_emit_mesh(&dl, &c, &l, &in);

		assert(n == 1);
		assert(dl.n == 1);
		/* Well within the light's fog range and normal points straight at
		 * the light, so colour should be the FULLY LIT (not fogged, not
		 * ambient-only) tint of palette[2], i.e. not equal to the raw
		 * palette entry (lighting always attenuates unless intensity is
		 * exactly 1.0, which ambient 0.2 + full lambert can reach) --
		 * mainly this must not silently be 0 or the sentinel. */
		assert(dl.tri[0].c != 0);
		(void)tr_r3d_palette;
	}

	/*
	 * 7. tr_sincosf() (in-repo, IEEE basic ops, replaces libm cosf/sinf so
	 * host and A32 front-ends compute the same bits): max abs error vs
	 * double sin/cos over [-50, 50] rad < 2e-7 (measured 8.1e-8), exact at 0.
	 */
	{
		float  s0, c0;
		double worst = 0.0;

		tr_sincosf(0.0f, &s0, &c0);
		assert(s0 == 0.0f && c0 == 1.0f);
		for (int i = -500000; i <= 500000; i++) {
			float  x = (float)i * 1.0e-4f;
			float  s, c;
			double es, ec;

			tr_sincosf(x, &s, &c);
			es    = fabs((double)s - sin((double)x));
			ec    = fabs((double)c - cos((double)x));
			worst = es > worst ? es : worst;
			worst = ec > worst ? ec : worst;
		}
		printf("tr_sincosf max abs error %.3g\n", worst);
		assert(worst < 2.0e-7);
	}

	/*
	 * 8. tr_atan2f() (the leg IK, P3d): max abs error vs double atan2 over
	 * every direction and a range of magnitudes < 4e-7 rad; the axes and
	 * (0, 0) exact.
	 */
	{
		double worst = 0.0;

		assert(tr_atan2f(0.0f, 0.0f) == 0.0f && tr_atan2f(0.0f, 1.0f) == 0.0f);
		assert(tr_atan2f(1.0f, 0.0f) == 1.57079633f && tr_atan2f(-1.0f, 0.0f) == -1.57079633f);
		for (int i = 0; i < 200000; i++) {
			double a = -3.14159 + 6.28318 * (double)i / 200000.0, m = 0.01 + (double)(i % 97) * 3.0;
			float  y = (float)(m * sin(a)), x = (float)(m * cos(a));
			double e = fabs((double)tr_atan2f(y, x) - atan2((double)y, (double)x));

			worst = e > worst ? e : worst;
		}
		printf("tr_atan2f max abs error %.3g\n", worst);
		assert(worst < 4.0e-7);
	}

	return 0;
}
