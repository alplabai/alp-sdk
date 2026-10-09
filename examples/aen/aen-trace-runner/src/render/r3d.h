/* src/render/r3d.h -- real-3D display-list pipeline: camera, projection,
 * mesh/quad emission, and the rasterizer. See the real-3D plan
 * (docs/superpowers/plans/2026-09-22-real-3d-renderer.md), sections 2 and 5
 * (T3/T4), for the derivation of every constant below.
 *
 * Split, like proj.h/proj.c: r3d_math.c holds the float camera/projection/
 * emit maths (per-vertex work, done once per frame, FPU is cheap for it);
 * r3d_raster.c holds the fixed-point per-pixel rasterizer (span.h's Helium
 * fill is the only thing that has to be fast). Both are pure C, free of
 * Zephyr/alp-sdk headers -- tests/host/runner.sh compiles them into every
 * host test, same convention as proj.c.
 *
 * Coordinate conventions, fixed for every function below:
 *   - World / camera space: tr_v3_t, float, x right, y up, z depth
 *     (larger z == farther, matching proj.c's z -- NOT the screen y this
 *     module's own tr_sv_t.y is, which is unrelated).
 *   - Screen space: 28.4 fixed point (TR_R3D_SUB fractional bits), x right
 *     0..TR_R3D_W*16, y DOWN 0..TR_R3D_H*16 (raster convention, opposite of
 *     world/camera y).
 *   - Triangle winding: a triangle is front-facing, and the rasterizer
 *     fills it, exactly when its screen-space signed area
 *     (v1-v0) x (v2-v0) (the edge() cross product in r3d_raster.c) is
 *     POSITIVE -- clockwise on screen, since screen y is down (this is the
 *     same triangle direction the classic y-down top-left-rule rasterizer
 *     algorithm is built around). A zero-or-negative area is both a
 *     backface AND a degenerate/zero-area triangle in one test, which is
 *     why tr_r3d_emit_quad()/tr_r3d_emit_mesh() and tr_raster_tri() both
 *     use it for cull -- see their comments.
 */
#ifndef TR_R3D_HEADER_H
#define TR_R3D_HEADER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Portrait framebuffer: spans run along the 800-px scanout rows (section 1
 * of the plan -- a landscape presentation rotates the physical mount, never
 * the raster). 800 is the widest panel (the Riverdi RVT121, 800 x 1280 scanned
 * turned); a narrower panel (the RK055, 720) gets the centre fw columns of the
 * same render (tr_frame_in_t.fw, panel_rot.h) -- the picture is drawn natively
 * at 800 and cropped, never stretched. A multiple of 16: the band copy and the
 * NEON kernels move whole 16-byte stores. */
#define TR_R3D_W 800
#define TR_R3D_H 1280
_Static_assert(TR_R3D_W % 16 == 0, "the band copy-out moves 16 px at a time");

/* Viewport split (maintainer ruling 2026-10-08, 3/5 game : 2/5 camera, one rule for every
 * panel; supersedes "Half / half" and fix round 8's 853/427): the physical panel stays
 * 800x1280 (TR_R3D_W/H, the CDC200 scanout, the framebuffer size, DMA), the 3D GAME fills the
 * top TR_VIEW_H rows (768 = 3/5), and rows [TR_VIEW_H, TR_R3D_H) are the video area (512 =
 * 2/5; a32/renderer/render.c render_video_band()/render_video_overlay(): the camera scaled up
 * to cover it, its skeleton, the intent lamps and the "CAMERA . NPU Hz" label on a plate at
 * its bottom edge). Everything that frames the GAME on
 * screen derives from this ONE constant: the projection centre (r3d_math.c
 * cy), the camera focal length (r3d_scene.h TR_CAM_F_PX), the horizon
 * clamp, the raster/band clip and the flash border. Gameplay (scroll speed,
 * ground_y, collision) stays in world units -- tr_runner_ground_y(TR_R3D_H)
 * -- and never reads TR_VIEW_H (fix round 9's lesson). */
#define TR_VIEW_H 768 /* 3/5 of 1280, TR_BAND_H-aligned: 24 full bands, no ragged last band */
_Static_assert(TR_VIEW_H * 5 == TR_R3D_H * 3 && TR_VIEW_H % 32 == 0, "3/5 game, whole bands");
/* The viewport height the camera and the on-screen size targets (the
 * visibility lead, the framing bounds) were tuned at (fix round 9), and a
 * screen-px figure from that tuning rescaled to TR_VIEW_H. */
#define TR_VIEW_TUNED_H 853
#define TR_VIEW_PX(px)  ((px) * TR_VIEW_H / TR_VIEW_TUNED_H)

/* 28.4 fixed point for screen-space vertices and the raster's edge maths --
 * bit-exact between host and target, unlike float, so host-built goldens
 * mean something (plan section 2, "Numeric"). */
#define TR_R3D_SUB 4

/*
 * Guard band: projected screen coordinates are clamped to +-16,384 px
 * (in 28.4 units) before they ever reach the rasterizer. This is not a
 * screen clip (that happens per-pixel in tr_raster_tri(), against the real
 * 0..TR_R3D_W/H bounds) -- it exists so a vertex just past the near plane,
 * where perspective divide blows up, can't hand the rasterizer a
 * coordinate whose 28.4 value overflows the int64 edge-function maths in
 * r3d_raster.c, or whose bounding box is so large the per-row scan in
 * tr_raster_tri() would take an unbounded time on one enormous off-screen
 * triangle.
 */
#define TR_R3D_GUARD (16384 << TR_R3D_SUB)

/* A32 budget (a32-renderer plan section 7): 4096 tris x 52 B = 208 KiB DL;
 * P16b 4608 (234 KiB DL, 630 KiB setup records -- render.c's memory-map
 * asserts hold): the high-LOD runner took the synthetic worst case (16
 * wires and obstacles packed near, crashing) past 4096.
 * NPU body control (docs/superpowers/specs/2026-09-24-npu-body-control-design.md
 * sec 2/9): 4608 -> 4528 (-80) first, then -> 4489 (-39 more) once real
 * silicon (the HP ram console) proved the arena itself needed to grow:
 * "Failed to resize buffer. Requested: 284288, available 283864, missing:
 * 424." (alp_inference_open ALP_ERR_IO) -- TFLM's own persistent
 * allocations sit ON TOP of Vela's 277.5 KiB SRAM figure, which was never
 * the whole story. Ruled arena size 0x46000 (286,720 B, tr_memmap.h
 * TR_MEM_NPU_ARENA_SIZE) -- about 2.4 KiB headroom over the 284,288 B TFLM
 * actually asked for. 4608 -> 4489 is -119 total (-2.58%). Any part-2
 * triangles past the cap are dropped and counted (render.c's existing
 * tr_dl_dropped), not corrupted -- still well above the 4096 P16b was
 * raised past.
 * ponytail: an untested 2.58% cut to the global tri budget, and the
 * arena's 2.4 KiB headroom is itself untested against a real second frame
 * (TFLM's persistent allocation could still grow with the model or a TFLM
 * upgrade) -- bench-check render_stats.tr_dl_dropped on the packed-crash
 * worst case, and if alp_inference_open ever fails again, the HP ram
 * console's "Failed to resize buffer... missing: N" line names the exact
 * shortfall to add to TR_MEM_NPU_ARENA_SIZE. */
#define TR_DL_MAX_TRIS  4489
#define TR_DL_MAX_VERTS 3072

/* Band raster (a32-renderer plan section 7): 40 bands of 32 rows, each band
 * owns a TR_R3D_W x 32 uint16 z band. A band's bin holds <= TR_BIN_MAX DL
 * indices; a triangle that does not fit is counted in *overflow, not drawn
 * in that band. 1024 -> 1536 with the far road and scenery out to the
 * skyline: the horizon's band holds everything past ~7,000 deep (worst in
 * play 940, tests/host/test_scene_load.c); 120 KiB, the stacks moved up to
 * fit (tr_memmap.h, render.c asserts). */
#define TR_BAND_SHIFT 5
#define TR_BAND_H     (1 << TR_BAND_SHIFT)
#define TR_BANDS \
	((TR_VIEW_H + TR_BAND_H - 1) / TR_BAND_H) /* ceiling: covers the ragged last band */
#define TR_BIN_MAX 1536

/* Textures: RGB565, TR_TEX_DIM x TR_TEX_DIM, row-major; a tri's `tex`
 * indexes tr_r3d_tex[] (set by the caller, TR_TEX_MAX slots). u/v are 8.8
 * texel coordinates (0x8000 == TR_TEX_DIM texels, one full width), wrapped
 * with & (TR_TEX_DIM - 1): nearest sampling, no filtering. */
#define TR_TEX_DIM 128
#define TR_TEX_MAX 8
/* Textured spans are perspective-correct every TR_TEX_SUB px (one divide),
 * affine in between. Part of the image: goldens are built with 8. (16 was
 * measured under emulation: no fewer A32 instructions -- the 8-px texel
 * gather, not the divide, is the cost.) */
#ifndef TR_TEX_SUB
#define TR_TEX_SUB 8
#endif
extern const uint16_t *tr_r3d_tex[TR_TEX_MAX];

/* tr_tri_t.flags */
#define TR_TRI_GOURAUD 0x01u /* per-pixel RGB interpolated from a[k].rgb */
#define TR_TRI_TEX     0x02u /* textured from tr_r3d_tex[tex]; wins over GOURAUD */
/* Background layer (the ground plane): no z test, no z write. tr_bin_only()
 * bins every NOZ triangle of a band ahead of the rest, so the band's z is
 * still the cleared "far" when they draw and everything after z-tests
 * against that. Only for geometry nothing can be behind (y = 0 seen from
 * above). Where NOZ triangles overlap, the later in DL order wins (the scene
 * tucks the board under the shoulder texture that way). */
#define TR_TRI_NOZ 0x04u
/* Textured, long quad: u/v hold the 8.8 texel coordinate / 8 (1/32 texel,
 * 16 texture widths of range) and a[k].rgb (unused by a texture) holds the
 * 16 fraction bits of a[k].w. One quad spanning many texture repeats instead
 * of one per repeat: the extra w bits keep its perspective as exact as the
 * short quads' (measured: integer w nearly doubles the texel error of a
 * quad 10 repeats deep). Precondition: (16 u + 1) * w * 2^16 < 2^49 at every
 * vertex (u, v the stored values, w with its fraction) -- true where u/v
 * grow only as w shrinks, like a ground quad; tri_setup() drops a triangle
 * that breaks it (not drawn). */
#define TR_TRI_UVX8 0x08u

/* Near-clip plane, view-space z (camera-forward depth), world units --
 * chosen in the plan alongside TR_PROJ_Z_NEAR's reasoning (proj.h): stop
 * well short of a perspective-divide blow-up. */
#define TR_CAM_Z_NEAR 32.0f

typedef struct {
	float x, y, z;
} tr_v3_t; /* x right, y up, z = depth (proj.c's z) */

/* World-to-camera transform, combined rotation + translation: for a world
 * point w, camera-space p[row] = m[row][0]*w.x + m[row][1]*w.y +
 * m[row][2]*w.z + m[row][3]. Row 2 (z) is camera-forward depth. Built by
 * tr_cam_build(); see r3d_math.c for the derivation. */
typedef struct {
	float m[3][4];
} tr_m34_t;

typedef struct {
	tr_m34_t view;
	float    f_px, cx, cy; /* focal length + screen-space projection centre, px */
} tr_cam_t;

typedef struct {
	int32_t x, y; /* 28.4 fixed, screen space, y down */
} tr_sv_t;

/*
 * Per-vertex attributes. w = 65535 * TR_CAM_Z_NEAR / view_z (clamped to
 * 1..65535): 1/z scaled so the near plane is 65535 -- NEARER IS LARGER, and
 * because 1/z is affine in screen space it is interpolated linearly (plane
 * equation) with no per-pixel divide. u/v: 8.8 texel coordinates (see
 * TR_TEX_DIM). rgb: lit + fogged RGB565 at this vertex (Gouraud input).
 */
typedef struct {
	uint16_t w, u, v, rgb;
} tr_vattr_t;

typedef struct {
	tr_sv_t    v[3];
	uint16_t   c;     /* final RGB565, already lit + fogged -- see tr_r3d_emit_*() */
	uint8_t    tex;   /* tr_r3d_tex[] slot, read only with TR_TRI_TEX */
	uint8_t    flags; /* TR_TRI_* */
	tr_vattr_t a[3];  /* attributes of v[0..2] */
} tr_tri_t;

typedef struct tr_dl_s {
	tr_tri_t tri[TR_DL_MAX_TRIS];
	uint16_t n;
	/* A display list in two pieces without copying one onto the other (the renderer's two cores
	 * build one each): triangles [0, split) are tri[], [split, n) are tail->tri[0, n - split).
	 * tail NULL (a freshly zeroed list, a one-core frame): all of it is tri[]. Readers go through
	 * tr_dl_tri(); whoever appends to tri[] (the scene build) never sees the tail. */
	uint16_t              split;
	const struct tr_dl_s *tail;
} tr_dl_t;

static inline const tr_tri_t *tr_dl_tri(const tr_dl_t *dl, uint32_t i)
{
	return dl->tail != NULL && i >= dl->split ? &dl->tail->tri[i - dl->split] : &dl->tri[i];
}

/*
 * Per-triangle raster setup, built ONCE per triangle by tr_bin_build() (all
 * int64 divides live there) and read by every band the triangle touches;
 * a band entry is int32 adds only (see r3d_raster.c). Internal layout --
 * callers only allocate it: 140 B each, TR_DL_MAX_TRIS of them 560 KiB,
 * which on the A32 goes in SRAM1 MiB 2 (0x02600000, WB, S=1; see the
 * a32-renderer plan's memory map), not in the renderer .bss.
 */
typedef struct {
	int32_t quot, rem, step_quot, step_rem, denom; /* exact rational row bound */
	int32_t q32, r32;                              /* the same step x TR_BAND_H rows */
} tr_edge_dda_t;

typedef struct {
	uint32_t row;    /* attribute at (ax, y0), uint32 modular fixed point */
	uint32_t gx, gy; /* per-pixel / per-row steps */
} tr_plane_t;

typedef struct {
	int16_t       y0, y1; /* rows drawn: [y0, y1); y0 >= y1 == nothing. Only
				 * rows that meet screen columns [0, TR_R3D_W). */
	int16_t       ax;     /* plane anchor column */
	uint8_t       ne;     /* edges in e[] (horizontal edges fold into y0/y1) */
	uint8_t       nr;     /* e[0..nr) are the right bounds, e[nr..ne) the left */
	tr_edge_dda_t e[3];   /* each at row y0 */
	tr_plane_t    pl[4];  /* w, then r, g, b (Gouraud) or u*w, v*w (textured) */
} tr_tri_setup_t;

/*
 * A mesh's raw geometry, as authored (T5's tools/genmesh.py -> meshes.h).
 * FLAT shading, one normal and one colour per TRIANGLE, not per vertex:
 *   - v:   nv vertices, 3x int16_t each (x, y, z), local mesh-space units
 *          on the same scale as world units (TR_TILE_LEN etc.) -- no
 *          separate model scale factor, tr_inst_t.scale_y is the only
 *          per-instance scale and it only ever squashes y (duck pose).
 *   - tri: nt triangles, 3x uint8_t each -- indices into v, so nv <= 256.
 *   - n:   nt triangles, 3x int8_t each (nx, ny, nz) -- a unit face normal
 *          scaled by 127 (127 == 1.0), e.g. straight-up-facing (0,127,0).
 *   - col: nt uint8_t, one PALETTE INDEX per triangle, 0..15, resolved
 *          against tr_r3d_palette (below) -- NOT a packed RGB565 value.
 *          Ruled on for this task (T4): the plan leaves col's meaning
 *          open ("uint8 per-tri colour index or RGB -- pick one"); a
 *          single byte can't hold RGB565 (needs 2), and the codebase
 *          already has exactly this convention for sprite art (see
 *          sprite.h's 16-entry `const uint16_t palette[16]` and atlas.h's
 *          "Shared 16-entry RGB565 palette") -- a mesh's colour byte
 *          follows the same shape, letting T5's genmesh.py reuse (or a
 *          later task widen to) the same palette machinery instead of
 *          inventing a second colour representation.
 */
typedef struct {
	const int16_t  *v;
	const uint8_t  *tri;
	const int8_t   *n;
	const uint8_t  *col;
	uint16_t        nv, nt;
	const int8_t   *vn;   /* nv per-VERTEX normals (x,y,z, 127 == 1.0) for
			    * TR_TRI_GOURAUD; NULL -> the face normal at all 3
			    * vertices (Gouraud then only carries per-vertex fog). */
	const float    *vf;   /* non-NULL: nv float xyz used instead of v -- a mesh
			    * posed per frame (the skinned runner, r3d_scene.c);
			    * generated meshes leave it NULL. */
	const uint16_t *pal;  /* 16 RGB565 colours `col` indexes instead of
			      * tr_r3d_palette (NULL): a runner character's own (P16) */
	uint16_t        emis; /* bit c: palette colour c is emissive -- drawn at
			       * full intensity, unlit (still fogged): eyes, glow strips */
} tr_mesh_t;

/*
 * One instance of a mesh in the world. Rotation is yaw-only (about world
 * y) and scale is y-only -- every prop in the plan's scene (caps, DIPs,
 * the runner, pickups) only ever spins upright or squashes vertically
 * (duck pose, jump squash); nothing in the design tilts or scales x/z, so
 * a full 3x3 rotation + non-uniform scale would be unused generality.
 */
typedef struct {
	const tr_mesh_t *mesh;
	tr_v3_t          pos;
	float            yaw;
	float            scale_y;
	uint8_t          flags; /* TR_TRI_GOURAUD: light per vertex, not per face;
				 * TR_TRI_NOZ: passed to every triangle (ground) */
	float            pitch; /* tip about the mesh's local x at its origin (the feet), before yaw,
				 * radians; negative tips +y toward -z. 0 = upright (the crash tumble
				 * is the one user; 0 costs nothing and changes no bits). */
} tr_inst_t;

/*
 * `dir`: unit vector FROM a lit surface TOWARD the light (not the
 * direction the light travels) -- so a face's intensity is
 * max(dot(face_normal, dir), 0) * (1 - ambient) + ambient, clamped to
 * [0, 1], then lerped toward `fog` (RGB565 already, section 2's "quantise
 * once per triangle") by view z across [fog_start, fog_end].
 */
typedef struct {
	tr_v3_t dir;
	float   ambient;
	uint8_t fog[3]; /* fog colour, 8-bit R,G,B (not RGB565 -- lerped in 8-bit first) */
	float   fog_start, fog_end;
	/* The 16-entry palette a mesh's col bytes index (a world zone's, P15);
	 * NULL = tr_r3d_palette. */
	const uint16_t *pal;
	/* fog_far > 0: past view z fog_knee the haze stops building along the
	 * curve above and runs on linearly from its amount there to 1 at
	 * fog_far -- the near scene hazed as ever, the far world readable out
	 * to the horizon (the world's lights, r3d_scene.c). 0: the curve alone. */
	float fog_knee, fog_far;
} tr_light_t;

/*
 * Shared 16-entry RGB565 base-colour palette every mesh triangle's `col`
 * byte (0..15) indexes into, before Lambert + fog are applied -- see
 * tr_mesh_t's doc comment above. Defined in r3d_math.c: tools/genart.py's
 * PCB palette, the indices tools/genmesh.py authors meshes.h against.
 */
extern const uint16_t tr_r3d_palette[16];

/* Triangles tr_r3d_emit_mesh()/tr_r3d_emit_quad() refused to append because
 * the display list was already at TR_DL_MAX_TRIS -- see their comments.
 * Never cleared by this module; a caller (T6's tr_scene_build(), later)
 * resets it once per frame alongside dl->n if it wants a per-frame count. */
extern uint32_t tr_dl_dropped;

/* sin and cos of x radians from IEEE basic ops only (host == target bits;
 * see r3d_math.c for range and error). */
void tr_sincosf(float x, float *s, float *c);

/* atan2(y, x) in radians, (-pi, pi], from IEEE basic ops only (the runner's
 * leg IK, r3d_rig.c); 0 for (0, 0). Max abs error 2.74e-7 rad (test_r3d_math: < 4e-7). */
float tr_atan2f(float y, float x);

/* Builds a world-to-camera transform for a camera at `eye`, facing the
 * direction (yaw, pitch) with bank `roll`, all radians, projecting with
 * focal length `f_px` px. cx/cy (the screen-space projection centre) are
 * set to the frame centre (TR_R3D_W/2, TR_R3D_H/2) -- a scene that wants
 * the horizon off-centre adjusts cx/cy on the returned cam_t afterward. */
void tr_cam_build(tr_cam_t *c, tr_v3_t eye, float yaw, float pitch, float roll, float f_px);

/*
 * Projects world point `w` through camera `c`. Always writes *view_z (the
 * camera-space depth, before any clamp) even when not visible -- a caller
 * doing near-plane clipping (tr_r3d_emit_mesh/quad) needs that value to
 * interpolate a clip vertex even though this point itself is behind the
 * near plane. Returns false, and leaves *out untouched, when
 * *view_z < TR_CAM_Z_NEAR (the point is at or behind the near plane, or
 * the perspective divide would be unstable); returns true and writes *out
 * (28.4 screen space, guard-band clamped) otherwise.
 */
bool tr_r3d_project(const tr_cam_t *c, tr_v3_t w, tr_sv_t *out, float *view_z);

/*
 * Transforms, lights, fogs and near-clips every triangle of mesh `in->mesh`
 * and appends the result to `dl`. Backface-culled per triangle (screen-
 * space signed area <= 0, see this file's top comment); near-clipped in
 * view space against TR_CAM_Z_NEAR (0, 1 or 2 output triangles per input
 * triangle, depending how many of its 3 vertices are behind the plane);
 * each surviving triangle is guard-band clamped and flat-shaded once
 * (Lambert + fog -> RGB565) before being written to dl->tri[dl->n++].
 * Refuses (appends nothing, counts every triangle it would have appended
 * in tr_dl_dropped, does not partially fill dl) once dl->n would reach
 * TR_DL_MAX_TRIS. Returns the number of triangles actually appended.
 * Every tri carries per-vertex w (for the z band) and a[k].rgb; with
 * in->flags & TR_TRI_GOURAUD the rgb is lit + fogged PER VERTEX (vertex
 * normal from mesh->vn, view z of that vertex) and the tri is flagged
 * Gouraud; otherwise all 3 carry the flat face colour `c`.
 */
uint16_t tr_r3d_emit_mesh(tr_dl_t *dl, const tr_cam_t *c, const tr_light_t *l, const tr_inst_t *in);

/*
 * Same emission pipeline as tr_r3d_emit_mesh(), for one already-lit quad
 * (4 world-space corners q[0..3], wound so (q0,q1,q2) and (q0,q2,q3) are
 * both front-facing when the quad itself faces the camera) with a single
 * caller-supplied RGB565 colour -- ground/wall tiles authored flat in
 * r3d_scene.c (T6), not through a tr_mesh_t. No lighting/fog is applied
 * (the plan calls these "pre-lit tiles"); still backface-culled, near-
 * clipped and guard-band clamped exactly like emit_mesh. Returns the
 * number of triangles appended (0, 1 or 2 per quad after near-clip).
 * `flags`: TR_TRI_* the triangles carry (TR_TRI_NOZ for ground; 0 usual).
 */
uint16_t tr_r3d_emit_quad(tr_dl_t        *dl,
                          const tr_cam_t *c,
                          const tr_v3_t   q[4],
                          uint16_t        rgb565,
                          uint8_t         flags);

/* tr_r3d_emit_quad() with a texture: corner q[k] gets 8.8 texel coords
 * uv[k] = {u, v} (/ 8 with TR_TRI_UVX8 in `flags`); tris carry TR_TRI_TEX |
 * flags and slot `tex`. Near-clip interpolates u/v in camera space, so the
 * clipped tri still maps right. */
uint16_t tr_r3d_emit_quad_tex(tr_dl_t        *dl,
                              const tr_cam_t *c,
                              const tr_v3_t   q[4],
                              const uint16_t  uv[4][2],
                              uint8_t         tex,
                              uint8_t         flags);

/* tr_r3d_emit_quad() with a colour per corner (rgb[k] at q[k]), Gouraud:
 * near-clip interpolates the colour like u/v. */
uint16_t tr_r3d_emit_quad_rgb(tr_dl_t        *dl,
                              const tr_cam_t *c,
                              const tr_v3_t   q[4],
                              const uint16_t  rgb[4],
                              uint8_t         flags);

/*
 * Fills screen rows [0, horizon_y) (clamped to [0, TR_R3D_H]) of `fb`
 * (stride `stride_px`, TR_R3D_W px wide) with a vertical gradient from
 * `top` (row 0) to `bot` (row horizon_y - 1), one tr_span_fill() call per
 * row -- the "sky gradient ... Helium row fills" line item in the plan's
 * per-frame budget (section 2). Rows >= horizon_y are untouched (the
 * ground/scene draw covers them).
 */
void tr_r3d_sky(uint16_t *fb, uint32_t stride_px, int32_t horizon_y, uint16_t top, uint16_t bot);

/*
 * Rasterizes one triangle into `fb` (stride `stride_px`), clipped to
 * [0, TR_R3D_W) x [0, TR_R3D_H) -- the screen clip named in plan section 2,
 * separate from and in addition to the guard-band clamp already applied
 * when `t` was emitted. Writes nothing for a zero-or-negative-area
 * triangle (backface or degenerate -- see this file's top comment) or one
 * whose clipped bounding box is empty (fully off-screen). Top-left fill
 * rule: a pixel on a shared edge between two triangles is filled by
 * exactly one of them, never both and never neither -- see r3d_raster.c.
 */
void tr_raster_tri(uint16_t *fb, uint32_t stride_px, const tr_tri_t *t);

/* Rasterizes every triangle in `dl`, in order -- painter's algorithm, no
 * z-buffer, flat colour `c` only (the M55 path). The A32 path is
 * tr_bin_build() + tr_raster_band(). */
void tr_r3d_draw(uint16_t *fb, uint32_t stride_px, const tr_dl_t *dl);

/*
 * Builds setup[i] for every DL triangle (edge DDAs + attribute planes: the
 * only per-triangle divides) and bins it into the bands its drawn rows
 * [y0, y1) touch. bins[b][0..counts[b]) are DL indices: the band's
 * TR_TRI_NOZ triangles in DL order, then the rest in DL order.
 * counts[] is reset here; *overflow is NOT: it ACCUMULATES one count per
 * (triangle, band) pair that found band b already full (that pair is not
 * drawn) -- the caller zeroes it per frame (mailbox out_dropped).
 */
void tr_bin_build(const tr_dl_t  *dl,
                  tr_tri_setup_t *setup,
                  uint16_t        bins[TR_BANDS][TR_BIN_MAX],
                  uint32_t        counts[TR_BANDS],
                  uint32_t       *overflow);
/* tr_bin_build() in two phases, so the setup can be split across cores:
 * tr_tri_setup_range() builds setup[lo..hi) (each record independent of the
 * others -- any split, any order, same bits); tr_bin_only() then bins all
 * dl->n in DL order from the finished records. Same result as
 * tr_bin_build(). */
void tr_tri_setup_range(const tr_dl_t *dl, tr_tri_setup_t *setup, uint32_t lo, uint32_t hi);
void tr_bin_only(const tr_dl_t        *dl,
                 const tr_tri_setup_t *setup,
                 uint16_t              bins[TR_BANDS][TR_BIN_MAX],
                 uint32_t              counts[TR_BANDS],
                 uint32_t             *overflow);

/*
 * Band background. fx == 0: tr_r3d_sky()'s gradient top -> bot over screen
 * rows [0, horizon), flat `ground` below (under whatever geometry covers it).
 * fx & TR_BG_DITHER: the sky is top -> mid (at mid_q8 / 256 of the way down;
 * mid_q8 0 = no mid stop) -> bot, interpolated in RGB565 channel units with 8
 * fraction bits and ordered-dithered by the 4x4 Bayer matrix (TR_BG_BAYER),
 * so a slow gradient shows no RGB565 bands. A row's dither pattern repeats
 * every 4 px: it is filled like a flat colour. Then, over the sky only:
 *   - halo_r > 0: a glow disc of radius halo_r px around (sun_x, sun_y),
 *     adding `halo` (RGB565 channel units) scaled by a radial falloff, capped
 *     per row so no channel saturates, dithered with the same matrix;
 *   - fx & TR_BG_STARS: TR_BG_STAR_N fixed stars, placed by a hash at a fixed
 *     height above the horizon row (they ride with it), fading in with height.
 */
#define TR_BG_DITHER 0x01u
#define TR_BG_STARS  0x02u
#define TR_BG_STAR_N 200
typedef struct {
	int32_t  horizon;
	uint16_t top, bot, ground;
	uint16_t mid;
	uint8_t  mid_q8;
	uint8_t  fx;
	int16_t  sun_x, sun_y; /* halo centre, screen px (may be off screen) */
	uint16_t halo_r;       /* px, <= 1023; 0 = no halo */
	uint16_t halo;         /* RGB565 added at the centre */
	uint8_t  star_dim;     /* stars at (256 - star_dim) / 256 strength (crash flash) */
} tr_bg_t;

/*
 * Fog for TR_TRI_NOZ textured spans (the near ground), per TR_TEX_SUB-px
 * sub-span: level = tr_r3d_fog_lut[w >> TR_FOG_W_SHIFT] (w = the sub-span's
 * first pixel, 0 past the table: nearer than the fog), then level 0 is the
 * plain texture and level l > 0 draws pal[l][idx[texel]] -- the texture as
 * palette indices, the palette pre-fogged per level. No per-pixel blend: one
 * extra byte load a pixel. A slot whose idx is NULL is never fogged. A slot
 * whose tr_r3d_tex[] is NULL but has idx is drawn through pal at every level,
 * level 0 too (pal row 0 = the plain colours): a texture stored as indices
 * only (the world zones, P15). TR_TRI_NOZ only -- a z-tested textured tri
 * needs tr_r3d_tex[].
 * tr_r3d_fog_build() fills both from a tr_light_t, with the same fog curve
 * tr_r3d_emit_mesh() applies per vertex, so fogged ground and fogged meshes
 * agree at any depth.
 */
#define TR_FOG_LEVELS  32
#define TR_FOG_PAL_MAX 32 /* colours a fogged texture may use */
#define TR_FOG_W_SHIFT 4
#define TR_FOG_LUT_N   512 /* covers w < 8192: view z > 256 */
typedef struct {
	const uint8_t  *idx; /* TR_TEX_DIM^2: palette entry * 2 (a byte offset) */
	const uint16_t *pal; /* TR_FOG_LEVELS rows of npal RGB565 */
	uint32_t        npal;
} tr_tex_fog_t; /* keyed by slot: rebuild after re-pointing tr_r3d_tex[slot]; one LUT, the last build's light */
extern tr_tex_fog_t tr_r3d_tex_fog[TR_TEX_MAX];
extern uint8_t      tr_r3d_fog_lut[TR_FOG_LUT_N];

/* Fog amount at camera depth view_z for light l, 0..1 (the curve every fogged
 * thing uses). */
float tr_r3d_fog_amount(const tr_light_t *l, float view_z);

/* The fogged palette rows of a texture whose n (<= TR_FOG_PAL_MAX) colours
 * are base[]: pal[lv * n + k] is base[k] lerped toward l->fog by
 * lv / (TR_FOG_LEVELS - 1), row 0 the colours themselves. */
void tr_r3d_fog_pal(const tr_light_t *l, const uint16_t *base, uint32_t n, uint16_t *pal);

/* (Re)builds tr_r3d_fog_lut from l's fog curve. */
void tr_r3d_fog_lut_build(const tr_light_t *l);

/*
 * Makes tr_r3d_tex[slot] fogged: idx (TR_TEX_DIM^2 B) and pal (TR_FOG_LEVELS *
 * TR_FOG_PAL_MAX) are caller storage; level l is every colour lerped toward
 * l->fog by l / (TR_FOG_LEVELS - 1). Also (re)builds tr_r3d_fog_lut from l.
 * False (slot left unfogged) if the texture has more than TR_FOG_PAL_MAX
 * colours or slot is empty.
 */
bool tr_r3d_fog_build(uint8_t slot, const tr_light_t *l, uint8_t *idx, uint16_t *pal);

/*
 * Renders screen rows [y_lo, y_hi): background (*bg) + DL triangles
 * bin[0..nbin) (their setup[] from tr_bin_build()) into the CACHED band
 * buffers -- `cband` colour and `zband` z, each (y_hi - y_lo) * TR_R3D_W
 * uint16, row y at (y - y_lo) * TR_R3D_W, both fully rewritten here -- then
 * copies the finished rows to `fb` (screen row 0, stride `stride_px`).
 * fb == NULL skips the copy: the caller copies `cband` itself (the A32
 * renderer does, to time it and to skip the TF-A MHU0 window in FB B).
 * The FB is only ever WRITTEN (full rows, 8-px stores on NEON): on the A32
 * it is Normal non-cacheable and reads there are the measured bus limit;
 * every z-tested read-modify-write hits the cached band instead. Coverage
 * is exactly tr_raster_tri()'s.
 *
 * Z test: a pixel is written when its interpolated w is STRICTLY greater
 * than zband (nearer). On an exact tie the triangle drawn first (lower bin
 * position) keeps the pixel -- coplanar overlap is resolved by DL order.
 * TR_TRI_NOZ triangles skip the test and never write zband (see the flag).
 *
 * Shading: flat `c`; TR_TRI_GOURAUD interpolates a[k].rgb per channel
 * (rounded, exact at vertex pixel centres); TR_TRI_TEX samples
 * tr_r3d_tex[tex] nearest, perspective-correct at every TR_TEX_SUB-px sub-span end
 * (one 32-bit divide + two SMULLs per sub-span end), affine inside it; a
 * TR_TRI_NOZ one is fogged per sub-span when its slot has tr_r3d_tex_fog.
 *
 * Edge DDAs and attribute planes come from setup[] (anchored per triangle,
 * not per band), so entering any band is int32 adds and any band split
 * renders bit-identically to one full-screen band. No 64-bit divide runs
 * here at all.
 */
void tr_raster_band(uint16_t             *fb,
                    uint32_t              stride_px,
                    int                   y_lo,
                    int                   y_hi,
                    uint16_t             *zband,
                    uint16_t             *cband,
                    const tr_bg_t        *bg,
                    const tr_dl_t        *dl,
                    const tr_tri_setup_t *setup,
                    const uint16_t       *bin,
                    uint32_t              nbin);

/*
 * Optional raster profile, build define TR_RASTER_PROF (the A32 renderer
 * sets it; host tests use it for pixel counts). The build supplies the two
 * hooks: tr_prof_core() -> this core's TR_PROF_N counters (never shared
 * between cores), tr_prof_now() -> a free-running cycle counter. Per entry:
 * cycles, pixels (or rows / triangles, see below), calls.
 *   FLAT/GOURAUD/TEX  z-tested spans of that kind: cycles, px, spans
 *   TRI               raster_setup() per (triangle, band): cycles incl.
 *                     its spans, rows, entries (overhead = TRI - spans)
 *   BG                band z clear + background: cycles, px, bands
 *   SETUP             tri_setup() in tr_tri_setup_range(): cycles, -, tris
 *   BIN               tr_bin_only(): cycles, -, tris
 *   NOZ_TEX           TR_TRI_NOZ textured spans: cycles, px, spans
 *   NOZ_FILL          TR_TRI_NOZ flat + Gouraud spans: cycles, px, spans
 */
enum {
	TR_PROF_FLAT,
	TR_PROF_GOURAUD,
	TR_PROF_TEX,
	TR_PROF_TRI,
	TR_PROF_BG,
	TR_PROF_SETUP,
	TR_PROF_BIN,
	TR_PROF_NOZ_TEX,
	TR_PROF_NOZ_FILL,
	TR_PROF_N
};
typedef struct {
	uint32_t cyc, px, n;
} tr_prof_t;
#ifdef TR_RASTER_PROF
tr_prof_t *tr_prof_core(void);
uint32_t   tr_prof_now(void);
#endif

/*
 * Boot self-check for the z-tested span loops this build compiled (NEON on
 * the A32) against the always-compiled scalar reference: flat, Gouraud and
 * textured, n 0..40 at every alignment 0..7, wrapping w steps; and the
 * TR_TRI_NOZ Gouraud/textured spans against the z-tested scalar ones on a
 * cleared z band. 1 on pass, 0 on the
 * first mismatch. The A32 renderer calls it once at entry beside
 * tr_span_selfcheck().
 */
int tr_raster_selfcheck(void);

#endif /* TR_R3D_HEADER_H */
