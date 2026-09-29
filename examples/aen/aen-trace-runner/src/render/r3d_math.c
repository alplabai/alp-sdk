/* src/render/r3d_math.c -- camera build, projection, and display-list
 * emission (backface cull, near-plane clip, guard-band clamp, flat
 * shading). See r3d.h for the contracts and coordinate conventions this
 * file implements, and the real-3D plan (docs/superpowers/plans/
 * 2026-09-22-real-3d-renderer.md, section 5, T4) for the task this is.
 *
 * All float, all per-vertex/per-triangle work done once a frame (plan
 * section 2, "Numeric") -- deliberately free of Zephyr/alp-sdk headers,
 * same convention as proj.c, so tests/host/runner.sh compiles this
 * straight into every host test.
 */
#include <stddef.h>

#include "r3d.h"

/* The PCB art palette: tools/genart.py's PALETTE (the sprite art) in RGB565,
 * index order identical so tools/genmesh.py names colours the same way
 * (MASK, COPPER, CHIP, ...). genart's index 0 is its colour key; here it is
 * solder-mask green instead, so a zeroed col byte still draws board. */
const uint16_t tr_r3d_palette[16] = {
	0x11A5, 0x0862, 0x11A5, 0x2B4A, 0xB325, 0xF5AC, 0xAD97, 0xEF9F,
	0x73F1, 0x2946, 0x4A4A, 0x2ADB, 0x7D5F, 0xEDD1, 0x5144, 0xFA4F,
};

uint32_t tr_dl_dropped;

/*
 * sin/cos from IEEE basic operations only (+ - * on float, one float->int
 * conversion), so the host test build and the A32 build compute the same
 * bits -- libm cosf/sinf differ between glibc and newlib, and the front-end
 * feeds the golden frame. Needs -ffp-contract=off everywhere (runner.sh and
 * every target Makefile) or a fused multiply-add changes the result.
 *
 * Cody-Waite reduction by pi/2 (hi part has 12 significant bits, so k * hi
 * is exact for |k| < 4096, i.e. |x| < ~6400 rad), then Taylor polynomials
 * on |r| <= pi/4 (truncation < 2e-9). Max abs error vs double sin/cos,
 * measured by test_r3d_math.c over [-50, 50]: 8.1e-8 (< 1 float ulp at 1).
 */
void tr_sincosf(float x, float *s, float *c)
{
	float    kf = x * 0.63661975f;
	int32_t  k  = (int32_t)(kf + (kf >= 0.0f ? 0.5f : -0.5f));
	float    r  = (x - (float)k * 1.5703125f) - (float)k * 4.8382679233e-4f;
	float    r2 = r * r;
	float    sn = r + r * r2 * (-1.6666667e-1f + r2 * (8.3333338e-3f + r2 * (-1.9841270e-4f + r2 * 2.7557319e-6f)));
	float    cs = 1.0f + r2 * (-0.5f + r2 * (4.1666668e-2f + r2 * (-1.3888889e-3f + r2 * (2.4801587e-5f +
										    r2 * -2.7557319e-7f))));
	uint32_t q  = (uint32_t)k & 3u;

	*s = q == 0 ? sn : q == 1 ? cs : q == 2 ? -sn : -cs;
	*c = q == 0 ? cs : q == 1 ? -sn : q == 2 ? -cs : sn;
}

/* atan on |t| <= tan(pi/8) (0.4142): the odd Taylor series to t^17
 * (truncation < 1.1e-8); beyond, atan(t) = pi/4 + atan((t - 1) / (t + 1))
 * for t <= 1 and pi/2 - atan(1/t) past it (via the min/max ratio). */
float tr_atan2f(float y, float x)
{
	float ax = x < 0.0f ? -x : x, ay = y < 0.0f ? -y : y;
	float mx = ax > ay ? ax : ay, mn = ax > ay ? ay : ax, t, r, off = 0.0f, t2;

	if (mx == 0.0f) {
		return 0.0f;
	}
	t = mn / mx; /* 0..1 */
	if (t > 0.41421356f) {
		t   = (t - 1.0f) / (t + 1.0f);
		off = 0.78539816f;
	}
	t2 = t * t;
	r  = off + t + t * t2 * (-1.0f / 3.0f + t2 * (0.2f + t2 * (-1.0f / 7.0f + t2 * (1.0f / 9.0f +
				    t2 * (-1.0f / 11.0f + t2 * (1.0f / 13.0f + t2 * (-1.0f / 15.0f + t2 * (1.0f / 17.0f))))))));
	if (ay > ax) {
		r = 1.57079633f - r;
	}
	if (x < 0.0f) {
		r = 3.14159265f - r;
	}
	return y < 0.0f ? -r : r;
}

/* lroundf() bit for bit (round half away from zero) for |f| < 2^31, inline:
 * newlib's is an out-of-line bit-twiddling call, 8 of them per emitted
 * vertex. (long)f truncates exactly, and f - trunc(f) is exact in float, so
 * the half-way test sees the true fraction. */
static inline long lround_f(float f)
{
	long  i = (long)f;
	float r = f - (float)i;

	if (r >= 0.5f) {
		i++;
	} else if (r <= -0.5f) {
		i--;
	}
	return i;
}

static float v3_dot(tr_v3_t a, tr_v3_t b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float clampf(float v, float lo, float hi)
{
	if (v < lo) {
		return lo;
	}
	if (v > hi) {
		return hi;
	}
	return v;
}

/*
 * tr_cam_build(): builds the combined world-to-camera rotation + translation
 * (see r3d.h's tr_m34_t comment). Camera-to-world rotation is composed
 * intrinsically roll-then-pitch-then-yaw (Rcw = Ryaw * Rpitch * Rroll: roll
 * spins the camera's own right/up axes about its forward axis first, pitch
 * then tilts that forward axis up/down about the camera's (post-roll)
 * local x, yaw then turns the whole rig about world y last) -- yaw/pitch/
 * roll are independent knobs a scene can drive from separate game state
 * (lane-follow yaw, jump pitch, bank roll -- plan section 1) without one
 * rotation's setting changing what another means.
 *
 * World-to-camera is Rcw's TRANSPOSE (rotation matrices are orthonormal,
 * so inverse == transpose -- cheaper than a general 3x3 invert and exact,
 * not approximate). The stored translation folds in -Rcw^T * eye once here
 * so tr_r3d_project() below is one matrix-vector multiply per vertex, not
 * a subtract-then-rotate.
 */
void tr_cam_build(tr_cam_t *c, tr_v3_t eye, float yaw, float pitch, float roll, float f_px)
{
	float cy, sy, cp, sp, cr, sr;

	tr_sincosf(yaw, &sy, &cy);
	tr_sincosf(pitch, &sp, &cp);
	tr_sincosf(roll, &sr, &cr);

	/* Ryaw * Rpitch * Rroll, expanded -- Rcw[row][col]. */
	float rcw[3][3] = {
		{cy * cr + sy * sp * sr, -cy * sr + sy * sp * cr, sy * cp},
		{cp * sr, cp * cr, -sp},
		{-sy * cr + cy * sp * sr, sy * sr + cy * sp * cr, cy * cp},
	};

	for (int row = 0; row < 3; row++) {
		for (int col = 0; col < 3; col++) {
			/* World-to-camera row `row`, col `col` == Rcw^T[row][col] ==
			 * Rcw[col][row]. */
			c->view.m[row][col] = rcw[col][row];
		}
		c->view.m[row][3] = -(rcw[0][row] * eye.x + rcw[1][row] * eye.y + rcw[2][row] * eye.z);
	}

	c->f_px = f_px;
	c->cx   = (float)TR_R3D_W / 2.0f;
	/* fix round 8: the projection's own vertical centre, TR_VIEW_H (the
	 * game viewport), not TR_R3D_H (the whole 720x1280 panel) -- see
	 * r3d.h's TR_VIEW_H comment. This is the single line that re-centres
	 * the whole scene's horizon/vanishing point for the shorter viewport;
	 * every other retune (ground row, horizon clamp, flash border) follows
	 * from tr_runner_ground_y(TR_VIEW_H) and clampi(.., TR_VIEW_H) call
	 * sites that were already parameterised on a screen-height argument. */
	c->cy = (float)TR_VIEW_H / 2.0f;
}

/* Transforms world point `w` into camera space using `view` -- shared by
 * tr_r3d_project() and the near-clip interpolation below (both need the
 * raw camera-space point, not just its projected screen position). */
static tr_v3_t to_camera(const tr_m34_t *view, tr_v3_t w)
{
	tr_v3_t out;

	out.x = view->m[0][0] * w.x + view->m[0][1] * w.y + view->m[0][2] * w.z + view->m[0][3];
	out.y = view->m[1][0] * w.x + view->m[1][1] * w.y + view->m[1][2] * w.z + view->m[1][3];
	out.z = view->m[2][0] * w.x + view->m[2][1] * w.y + view->m[2][2] * w.z + view->m[2][3];
	return out;
}

/* Projects an already-camera-space point; shared so the near-clip code
 * below (which interpolates new points directly in camera space) doesn't
 * have to round-trip back through a world-space point that doesn't exist
 * for a synthetic clip vertex. */
static tr_sv_t project_camera_space(const tr_cam_t *c, tr_v3_t p)
{
	float sx = c->cx + p.x * c->f_px / p.z;
	float sy = c->cy - p.y * c->f_px / p.z; /* camera y up -> screen y down */

	/*
	 * Clamp in PX (float), before scaling to 28.4 and rounding to
	 * int32_t: a point close to the camera's own x/y axis but at a small
	 * visible depth (view_z just past TR_CAM_Z_NEAR) can divide out to a
	 * screen position many orders of magnitude past the guard band --
	 * scaling THAT by 16 and rounding to int32_t first (as an earlier
	 * version of this function did) overflows int32_t before the clamp
	 * ever runs, which is undefined behaviour, not a clamp. Clamping the
	 * float pixel value first means the only value that ever reaches the
	 * int32_t cast is already inside +-TR_R3D_GUARD.
	 */
	float px_guard = (float)TR_R3D_GUARD / (float)(1 << TR_R3D_SUB);

	sx = clampf(sx, -px_guard, px_guard);
	sy = clampf(sy, -px_guard, px_guard);

	tr_sv_t out;

	out.x = (int32_t)lround_f(sx * (float)(1 << TR_R3D_SUB));
	out.y = (int32_t)lround_f(sy * (float)(1 << TR_R3D_SUB));
	return out;
}

bool tr_r3d_project(const tr_cam_t *c, tr_v3_t w, tr_sv_t *out, float *view_z)
{
	tr_v3_t p = to_camera(&c->view, w);

	*view_z = p.z;
	if (p.z < TR_CAM_Z_NEAR) {
		return false;
	}
	*out = project_camera_space(c, p);
	return true;
}

/*
 * A camera-space vertex plus everything the near clip has to carry across
 * the plane: texel coords (8.8, as float) and the Gouraud colour as 5/6/5
 * channel values (float so a clip vertex between two colours rounds once,
 * at pack time). Interpolating these linearly in CAMERA space is exact --
 * the rasterizer does the perspective part.
 */
typedef struct {
	tr_v3_t p;
	float   u, v, r, g, b;
} cvert_t;

static float lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

/*
 * Clips a camera-space triangle against the near plane, writing up to 4
 * output vertices (a clipped triangle can become a quad) to `out` and
 * returning how many. Standard Sutherland-Hodgman single-plane clip: walk
 * the triangle's 3 edges, keeping a vertex when it's in front
 * (z >= near) and inserting the plane-crossing point on any edge that
 * changes side.
 */
static int clip_near(const cvert_t cam[3], cvert_t out[4])
{
	int n = 0;

	for (int i = 0; i < 3; i++) {
		cvert_t a    = cam[i];
		cvert_t b    = cam[(i + 1) % 3];
		bool    a_in = a.p.z >= TR_CAM_Z_NEAR;
		bool    b_in = b.p.z >= TR_CAM_Z_NEAR;

		if (a_in) {
			out[n++] = a;
		}
		if (a_in != b_in) {
			/* Interpolate the exact plane crossing: t is where, along
			 * a->b, z == TR_CAM_Z_NEAR. a.z != b.z is guaranteed here
			 * (a_in != b_in means they're on opposite sides). */
			float t = (TR_CAM_Z_NEAR - a.p.z) / (b.p.z - a.p.z);

			out[n++] = (cvert_t){
				{lerpf(a.p.x, b.p.x, t), lerpf(a.p.y, b.p.y, t), lerpf(a.p.z, b.p.z, t)},
				lerpf(a.u, b.u, t),
				lerpf(a.v, b.v, t),
				lerpf(a.r, b.r, t),
				lerpf(a.g, b.g, t),
				lerpf(a.b, b.b, t),
			};
		}
	}
	return n;
}

/* Screen-space signed area, same edge() cross product r3d_raster.c's
 * top-left rule uses -- see r3d.h's top comment for the winding this
 * tests: > 0 is front-facing, <= 0 is backface-or-degenerate. int64_t:
 * guard-band coordinates (+-262144 in 28.4) multiply past int32. */
static int64_t signed_area(tr_sv_t a, tr_sv_t b, tr_sv_t c)
{
	return (int64_t)(b.x - a.x) * (int64_t)(c.y - a.y) - (int64_t)(b.y - a.y) * (int64_t)(c.x - a.x);
}

/* Appends one front-facing triangle to `dl`, honouring TR_DL_MAX_TRIS --
 * the one place every emit path funnels through, so the overflow-refusal
 * rule (r3d.h's tr_dl_dropped comment) lives in exactly one spot. `t`
 * arrives fully built; this only culls and copies. */
static bool dl_append(tr_dl_t *dl, const tr_tri_t *t)
{
	if (signed_area(t->v[0], t->v[1], t->v[2]) <= 0) {
		return false; /* backface or degenerate -- not a drop, just not drawn */
	}
	if (dl->n >= TR_DL_MAX_TRIS) {
		tr_dl_dropped++;
		return false;
	}
	dl->tri[dl->n++] = *t;
	return true;
}

static uint16_t pack565(float r, float g, float b)
{
	return (uint16_t)(((uint32_t)lround_f(r) << 11) | ((uint32_t)lround_f(g) << 5) | (uint32_t)lround_f(b));
}

static void unpack565(uint16_t c, cvert_t *v)
{
	v->r = (float)((c >> 11) & 0x1F);
	v->g = (float)((c >> 5) & 0x3F);
	v->b = (float)(c & 0x1F);
}

/* Per-vertex attributes from an already near-clipped camera-space vertex
 * (z >= TR_CAM_Z_NEAR, so w16 <= 65535 by construction). */
static uint16_t w16_of(float z)
{
	long w = lround_f(65535.0f * TR_CAM_Z_NEAR / z);

	return (uint16_t)(w < 1 ? 1 : (w > 65535 ? 65535 : w));
}

static tr_vattr_t vattr(const cvert_t *v, uint8_t flags)
{
	tr_vattr_t a = {
		w16_of(v->p.z),
		(uint16_t)lround_f(v->u),
		(uint16_t)lround_f(v->v),
		pack565(v->r, v->g, v->b),
	};

	if (flags & TR_TRI_UVX8) {
		/* w = floor, rgb = the 16 fraction bits (r3d.h, TR_TRI_UVX8);
		 * wf - floor(wf) is exact in float. */
		float wf = 65535.0f * TR_CAM_Z_NEAR / v->p.z;
		long  wi = (long)wf;
		long  fr = lround_f((wf - (float)wi) * 65536.0f);

		if (fr > 65535) {
			wi++, fr = 0;
		}
		a.w   = (uint16_t)(wi < 1 ? 1 : (wi > 65535 ? 65535 : wi));
		a.rgb = (uint16_t)(wi < 1 || wi > 65535 ? 0 : fr);
	}
	return a;
}

/* Projects the (already near-clipped, so every point has z >= TR_CAM_Z_NEAR
 * and this can never fail) camera-space polygon `cam[0..count)` and fans it
 * into (count - 2) triangles appended to `dl`, all sharing flat colour
 * `colour`, `flags` and `tex`. Fan triangulation is correct here
 * specifically because clip_near()'s output is always CONVEX. Returns how
 * many triangles were appended. */
static uint16_t emit_fan(tr_dl_t *dl, const tr_cam_t *c, const cvert_t cam[4], int count, uint16_t colour,
			 uint8_t flags, uint8_t tex)
{
	tr_sv_t    sv[4];
	tr_vattr_t va[4];
	uint16_t   emitted = 0;

	for (int i = 0; i < count; i++) {
		sv[i] = project_camera_space(c, cam[i].p);
		va[i] = vattr(&cam[i], flags);
	}
	for (int i = 1; i + 1 < count; i++) {
		tr_tri_t t = {{sv[0], sv[i], sv[i + 1]}, colour, tex, flags, {va[0], va[i], va[i + 1]}};

		if (dl_append(dl, &t)) {
			emitted++;
		}
	}
	return emitted;
}

static uint16_t emit_quad_common(tr_dl_t *dl, const tr_cam_t *c, const tr_v3_t q[4], const uint16_t uv[4][2],
				 uint16_t rgb565, const uint16_t *rgb4, uint8_t flags, uint8_t tex)
{
	static const uint8_t tris[2][3] = {{0, 1, 2}, {0, 2, 3}};
	uint16_t              emitted   = 0;

	for (int t = 0; t < 2; t++) {
		cvert_t cam[3], clipped[4];

		for (int k = 0; k < 3; k++) {
			uint8_t qi = tris[t][k];

			cam[k].p = to_camera(&c->view, q[qi]);
			cam[k].u = uv ? (float)uv[qi][0] : 0.0f;
			cam[k].v = uv ? (float)uv[qi][1] : 0.0f;
			unpack565(rgb4 ? rgb4[qi] : rgb565, &cam[k]);
		}
		emitted += emit_fan(dl, c, clipped, clip_near(cam, clipped), rgb565, flags, tex);
	}
	return emitted;
}

uint16_t tr_r3d_emit_quad(tr_dl_t *dl, const tr_cam_t *c, const tr_v3_t q[4], uint16_t rgb565, uint8_t flags)
{
	return emit_quad_common(dl, c, q, NULL, rgb565, NULL, flags, 0);
}

uint16_t tr_r3d_emit_quad_rgb(tr_dl_t *dl, const tr_cam_t *c, const tr_v3_t q[4], const uint16_t rgb[4], uint8_t flags)
{
	return emit_quad_common(dl, c, q, NULL, rgb[0], rgb, TR_TRI_GOURAUD | flags, 0);
}

uint16_t tr_r3d_emit_quad_tex(tr_dl_t *dl, const tr_cam_t *c, const tr_v3_t q[4], const uint16_t uv[4][2],
			      uint8_t tex, uint8_t flags)
{
	return emit_quad_common(dl, c, q, uv, 0, NULL, TR_TRI_TEX | flags, tex);
}

/* Flat Lambert + fog for one face, given its camera-space centroid depth
 * `view_z` (fog only, not lighting -- light.dir is a world-space
 * direction, unaffected by depth) and its WORLD-space unit normal
 * `wnormal` (already yaw-rotated by the caller, see tr_r3d_emit_mesh()).
 * Returns RGB565. */
static void shade_terms(const tr_light_t *l, tr_v3_t wnormal, float view_z, float *intensity, float *fog_t)
{
	float ndotl   = v3_dot(wnormal, l->dir);
	float lambert = clampf(ndotl, 0.0f, 1.0f);

	*intensity = clampf(l->ambient + (1.0f - l->ambient) * lambert, 0.0f, 1.0f);
	*fog_t     = tr_r3d_fog_amount(l, view_z);
}

/* Linear in depth across [fog_start, fog_end], eased out: t (2 - t) -- haze
 * builds quickly past fog_start, like real air, and still meets the fog
 * colour exactly at fog_end, where the scene stops drawing. With fog_far
 * set, past fog_knee it goes on linearly from there to 1 at fog_far. */
float tr_r3d_fog_amount(const tr_light_t *l, float view_z)
{
	int   far = l->fog_far > 0.0f && view_z > l->fog_knee;
	float t   = clampf(((far ? l->fog_knee : view_z) - l->fog_start) / (l->fog_end - l->fog_start), 0.0f, 1.0f);
	float a   = t * (2.0f - t);

	return far ? a + (1.0f - a) * clampf((view_z - l->fog_knee) / (l->fog_far - l->fog_knee), 0.0f, 1.0f) : a;
}

static uint16_t shade_apply(const tr_light_t *l, uint16_t base, float intensity, float fog_t)
{
	int r = (int)((base >> 11) & 0x1F);
	int g = (int)((base >> 5) & 0x3F);
	int b = (int)(base & 0x1F);

	r = (int)((float)r * intensity);
	g = (int)((float)g * intensity);
	b = (int)((float)b * intensity);

	/* fog[] is 8-bit R,G,B; base channels above are 5/6/5-bit, so widen
	 * both to 8-bit for the lerp and requantise once at the end -- lerping
	 * in the packed 5/6/5 domain would round two of the three channels
	 * against the wrong bit depth. */
	int r8 = (r << 3) | (r >> 2);
	int g8 = (g << 2) | (g >> 4);
	int b8 = (b << 3) | (b >> 2);

	r8 = (int)((float)r8 + ((float)l->fog[0] - (float)r8) * fog_t);
	g8 = (int)((float)g8 + ((float)l->fog[1] - (float)g8) * fog_t);
	b8 = (int)((float)b8 + ((float)l->fog[2] - (float)b8) * fog_t);

	return (uint16_t)(((r8 & 0xF8) << 8) | ((g8 & 0xFC) << 3) | (b8 >> 3));
}

/* Flat Lambert + fog for one face, given its camera-space centroid depth
 * `view_z` (fog only, not lighting -- light.dir is a world-space
 * direction, unaffected by depth) and its WORLD-space unit normal
 * `wnormal` (already yaw-rotated by the caller, see tr_r3d_emit_mesh()).
 * Returns RGB565. Split in two so a vertex's terms (normal + depth only)
 * are computed once per vertex, not once per triangle using it. */
static uint16_t shade(const tr_light_t *l, tr_v3_t wnormal, float view_z, uint16_t base)
{
	float in, ft;

	shade_terms(l, wnormal, view_z, &in, &ft);
	return shade_apply(l, base, in, ft);
}

/* Yaw-rotates (after tr_inst_t.pitch when `tip`) an int8 (127 == 1.0)
 * local normal into world space -- no translation, no scale_y (a duck
 * squash must not tilt the lighting). */
static tr_v3_t yaw_normal(const int8_t *n, float cy, float sy, float cp, float sp, bool tip)
{
	float nlx = (float)n[0] / 127.0f;
	float nly = (float)n[1] / 127.0f;
	float nlz = (float)n[2] / 127.0f;

	if (tip) { /* tr_inst_t.pitch, before the yaw */
		float y = nly * cp - nlz * sp;

		nlz = nly * sp + nlz * cp;
		nly = y;
	}
	return (tr_v3_t){nlx * cy + nlz * sy, nly, -nlx * sy + nlz * cy};
}

uint16_t tr_r3d_emit_mesh(tr_dl_t *dl, const tr_cam_t *c, const tr_light_t *l, const tr_inst_t *in)
{
	const tr_mesh_t *mesh    = in->mesh;
	float             cy, sy;
	uint16_t          emitted = 0;
	uint8_t           flags   = in->flags & (TR_TRI_GOURAUD | TR_TRI_NOZ);
	/* Per-instance vertex cache (meshes have nv <= 256, tri indices are
	 * uint8_t): each vertex transformed, projected, given its w and its
	 * Gouraud shade terms once, not once per triangle using it. On the
	 * stack (~7.5 KiB): the scene build runs on both A32 cores at once. */
	tr_v3_t  vc_cam[256];
	tr_sv_t  vc_sv[256];
	uint16_t vc_w[256];
	float    vc_in[256], vc_fog[256];

	float           cp = 1.0f, sp = 0.0f;
	bool            tip = in->pitch != 0.0f;
	const uint16_t *pal = l->pal ? l->pal : tr_r3d_palette;

	tr_sincosf(in->yaw, &sy, &cy);
	if (tip) {
		tr_sincosf(in->pitch, &sp, &cp);
	}

	/* The exact per-vertex expressions the per-triangle path used, so the
	 * DL is bit-identical (tests/host/tr_golden_dl.h pins it). */
	for (uint16_t i = 0; i < mesh->nv; i++) {
		float lx = mesh->vf ? mesh->vf[i * 3 + 0] : (float)mesh->v[i * 3 + 0];
		float ly = (mesh->vf ? mesh->vf[i * 3 + 1] : (float)mesh->v[i * 3 + 1]) * in->scale_y;
		float lz = mesh->vf ? mesh->vf[i * 3 + 2] : (float)mesh->v[i * 3 + 2];

		if (tip) {
			float y = ly * cp - lz * sp;

			lz = ly * sp + lz * cp;
			ly = y;
		}

		/* Yaw-only rotation about world y, then translate -- see
		 * r3d.h's tr_inst_t comment for why this is deliberately not
		 * a general rotation. */
		tr_v3_t wv = {lx * cy + lz * sy + in->pos.x, ly + in->pos.y, -lx * sy + lz * cy + in->pos.z};

		vc_cam[i] = to_camera(&c->view, wv);
		if (vc_cam[i].z >= TR_CAM_Z_NEAR) {
			vc_sv[i] = project_camera_space(c, vc_cam[i]);
			vc_w[i]  = w16_of(vc_cam[i].z);
		}
		if ((flags & TR_TRI_GOURAUD) && mesh->vn) {
			shade_terms(l, yaw_normal(&mesh->vn[i * 3], cy, sy, cp, sp, tip), vc_cam[i].z, &vc_in[i], &vc_fog[i]);
		}
	}

	for (uint16_t t = 0; t < mesh->nt; t++) {
		const uint8_t *ix    = &mesh->tri[t * 3];
		bool           front = vc_cam[ix[0]].z >= TR_CAM_Z_NEAR && vc_cam[ix[1]].z >= TR_CAM_Z_NEAR &&
				       vc_cam[ix[2]].z >= TR_CAM_Z_NEAR;

		/* No near clip: a backface costs nothing more (dl_append would
		 * refuse it anyway, and shading has no side effects). */
		if (front && signed_area(vc_sv[ix[0]], vc_sv[ix[1]], vc_sv[ix[2]]) <= 0) {
			continue;
		}

		cvert_t cam[3], clipped[4];

		for (int k = 0; k < 3; k++) {
			cam[k].p = vc_cam[ix[k]];
			cam[k].u = 0.0f;
			cam[k].v = 0.0f;
		}

		uint8_t  ci     = mesh->col[t] & 0xF;
		/* A character's own palette (P16), else the zone's (l->pal, P15),
		 * else tr_r3d_palette. */
		uint16_t base   = (mesh->pal ? mesh->pal : pal)[ci];
		bool     lit    = !((mesh->emis >> ci) & 1u);
		tr_v3_t  wn     = yaw_normal(&mesh->n[t * 3], cy, sy, cp, sp, tip);
		float    ctr_z  = (cam[0].p.z + cam[1].p.z + cam[2].p.z) / 3.0f;
		uint16_t shaded = lit ? shade(l, wn, ctr_z, base) : shade_apply(l, base, 1.0f, tr_r3d_fog_amount(l, ctr_z));
		uint16_t vcol[3];

		for (int k = 0; k < 3; k++) {
			vcol[k] = shaded;
			if (flags & TR_TRI_GOURAUD) {
				/* Per-vertex Lambert (vertex normal, or the face
				 * normal when the mesh has none) + per-vertex fog;
				 * an emissive colour takes the fog only. */
				vcol[k] = !lit       ? shade_apply(l, base, 1.0f, tr_r3d_fog_amount(l, cam[k].p.z))
					  : mesh->vn ? shade_apply(l, base, vc_in[ix[k]], vc_fog[ix[k]])
						     : shade(l, wn, cam[k].p.z, base);
			}
			unpack565(vcol[k], &cam[k]);
		}

		if (front) {
			/* == emit_fan() of the unclipped triangle: u = v = 0 and
			 * pack565(unpack565(c)) == c, so only w and rgb vary. */
			tr_tri_t tri = {{vc_sv[ix[0]], vc_sv[ix[1]], vc_sv[ix[2]]},
					shaded,
					0,
					flags,
					{{vc_w[ix[0]], 0, 0, vcol[0]}, {vc_w[ix[1]], 0, 0, vcol[1]}, {vc_w[ix[2]], 0, 0, vcol[2]}}};

			emitted += dl_append(dl, &tri) ? 1u : 0u;
		} else {
			emitted += emit_fan(dl, c, clipped, clip_near(cam, clipped), shaded, flags, 0);
		}
	}
	return emitted;
}

bool tr_r3d_fog_build(uint8_t slot, const tr_light_t *l, uint8_t *idx, uint16_t *pal)
{
	const uint16_t *tex = tr_r3d_tex[slot & (TR_TEX_MAX - 1)];
	uint16_t        col[TR_FOG_PAL_MAX];
	uint32_t        n = 0;

	tr_r3d_tex_fog[slot & (TR_TEX_MAX - 1)] = (tr_tex_fog_t){NULL, NULL, 0};
	if (tex == NULL) {
		return false;
	}
	for (uint32_t i = 0; i < TR_TEX_DIM * TR_TEX_DIM; i++) {
		uint32_t k = 0;

		while (k < n && col[k] != tex[i]) {
			k++;
		}
		if (k == n) {
			if (n == TR_FOG_PAL_MAX) {
				return false;
			}
			col[n++] = tex[i];
		}
		idx[i] = (uint8_t)(2u * k);
	}
	tr_r3d_fog_pal(l, col, n, pal);
	tr_r3d_fog_lut_build(l);
	tr_r3d_tex_fog[slot & (TR_TEX_MAX - 1)] = (tr_tex_fog_t){idx, pal, n};
	return true;
}

void tr_r3d_fog_pal(const tr_light_t *l, const uint16_t *base, uint32_t n, uint16_t *pal)
{
	for (uint32_t lv = 0; lv < TR_FOG_LEVELS; lv++) {
		for (uint32_t k = 0; k < n; k++) {
			pal[lv * n + k] = shade_apply(l, base[k], 1.0f, (float)lv / (float)(TR_FOG_LEVELS - 1));
		}
	}
}

void tr_r3d_fog_lut_build(const tr_light_t *l)
{
	/* w bin i covers w in [i, i + 1) << TR_FOG_W_SHIFT; judged at its centre. */
	for (uint32_t i = 0; i < TR_FOG_LUT_N; i++) {
		float w  = ((float)i + 0.5f) * (float)(1u << TR_FOG_W_SHIFT);
		float ft = tr_r3d_fog_amount(l, 65535.0f * TR_CAM_Z_NEAR / w);

		tr_r3d_fog_lut[i] = (uint8_t)(ft * (float)(TR_FOG_LEVELS - 1) + 0.5f);
	}
}
