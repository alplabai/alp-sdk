# Trace Runner — real-3D renderer plan (2026-09-22)

> **Status (2026-10-08): Landed** — the 3D renderer shipped in `examples/aen/aen-trace-runner` (#2452); sections 3-5 are superseded by `2026-09-22-a32-renderer.md`.

Status: plan. Every number marked (M) is measured in `docs/2026-09-22-measurements.md`; every number marked (E) is an estimate to be replaced by the CP2/CP3 counters below.

Core note (maintainer direction, 2026-09-22): bare-metal A32 is being brought up in parallel (BL33 payload under TF-A). Everything in T3-T6 is pure C and core-agnostic, so the same renderer can be retargeted to the A32 pair if the A32 probe (section 4) justifies it.

## 1. Game design that reads as "wow"

**Scene: a low-poly flight down a printed circuit board.** The existing art palette is already a PCB (solder-mask green `RGB565(18,52,42)`, copper `RGB565(176,100,40)`, chip `RGB565(40,40,48)`); the 3D world makes that literal, which is also exactly what a SoM customer is looking at.

- **Ground**: the board surface, 3 lanes + 2 shoulders (5 columns x TR_TILE_LEN 384 world units, 24 slices to Z_FAR 9216), alternating shade per slice so motion is visible; copper trace stripes down each lane (each tile split green/copper/green = 3 quads).
- **Walls**: ranks of tall components along both shoulders (electrolytic caps = 8-sided prisms, DIP ICs = boxes with legs, connectors) — ~24 tris per segment, ~6 visible per side.
- **Obstacles**: `low` = resistor lying across the lane (box + 2 leads, ~20 tris) — jump it; `high` = jumper-wire arch / bar on two posts at head height (~18 tris) — duck under it.
- **Pickups**: spinning copper via / solder coin, 8-sided prism (32 tris), yaw = tick-driven.
- **Runner**: small probe-robot, box body + head + two tick-animated legs (~60 tris); airborne = legs tucked, duck = squashed y-scale.
- **Sky**: vertical gradient (Helium row fills), a sun disc polygon, a parallax skyline of distant components (flat dark triangles shifting with camera x).
- **Lighting**: one directional light, Lambert flat shading per face + ambient; **depth fog** per triangle (lerp face colour to fog colour by view z, TR_FOG_START 1500 .. TR_FOG_END 8500). Quantise to RGB565 once per triangle.
- **Camera motion** (what separates real 3D from sprite scaling): eased x follow on lane change with bank roll +-TR_CAM_BANK_DEG 5, run bob (sin of tick), lift + pitch on jump, 2-frame dip on landing, slight focal-length kick with score/speed.
- **Particles**: 32 screen-space quads burst on pickup (solid, shrink to fade — no blending, blending would read the framebuffer).

**Per-frame budget**: TR_DL_MAX_TRIS 2048 after cull, TR_DL_MAX_VERTS 1536. Worst-case tally: ground 5x24x3x2 = 720, walls 12x24 = 288, entities 16x~40 = 640, runner 60, particles 64, sky/skyline ~40 -> ~1,800. Typical ~1,000.

**Presentation is portrait.** Spans run along the 720-px scanout rows; a landscape presentation needs the physical mount rotated, never a rotated raster (column-major writes would kill the Helium fill).

## 2. Rendering architecture

**Direct full-resolution rendering into the SRAM0 back buffer, no z-buffer, no upscale.**

Why, from the measurements: SRAM0 Helium stores 1,279.95 MB/s (M), SRAM0 reads 43-93 MB/s (M). A z-buffer is a read-modify-write per pixel (the 14x-slower side); an internal 360x640 colour+depth target is 921,600 B and does not fit the 1 MB HP DTCM beside anything else (nor the HE's 256 KB for the dev loop), and a banded version still writes the same 1.84 MB to SRAM0 in the upscale pass — it only pays off when per-pixel shading is expensive (textures/Gouraud/z-test), which this design avoids. Full-res also gives crisp edges.

- **Visibility**: painter's by construction. Emission order in `tr_scene_build()`: sky -> for slice k = 23..0: ground tiles(k), wall segments(k), entities with z in slice k -> runner -> particles. Proof the order is safe: a wall at world |x| >= 400 projects to |sx-360| = 400*f/z0; an entity at |x| <= 284 at z1 > z0 projects to <= 284*f/z1 < 400*f/z0 — no on-screen overlap between a wall slice and any lane entity farther away, so slice order is sufficient. Lanes are disjoint in x; the runner is always nearest. Convex meshes need only backface culling. Non-convex meshes (arch) are authored with their tris pre-sorted inside-first.
- **Rasterizer**: scanline edge-walk, top-left fill rule (no cracks between shared tile edges), screen clip to [0,720)x[0,1280), near-plane clip in view space (z < TR_CAM_Z_NEAR 32 -> 1-2 output tris), guard band +-16,384 px on projected coords. Spans are constant colour -> `tr_span_fill()` Helium.
- **Numeric**: **float** (M55 single-precision FPU) for camera build, transform, lighting, projection — a few thousand ops/frame; **integer 28.4** (`TR_R3D_SUB 4`) screen vertices and 16.16 edge slopes in the raster so the raster is bit-exact on the host and its goldens are meaningful. MVE-F not needed (per-vertex work ~0.5 ms); Helium only for span/row fills.
- **Shading**: flat per face. Gouraud is the named upgrade (3x span cost: three channels can't be interpolated inside packed RGB565 lanes).

**Per-frame budget on HP @400 MHz (25.0 ms frame)**

| stage | basis | ms |
|---|---|---|
| game step + input | today's HP render includes it, negligible | 0.2 |
| scene build + camera | ~60 objects, pure C | 0.3 (E) |
| transform + light + cull + clip | 1,536 verts x ~60 cyc + 2,048 tris x ~50 cyc | 0.5 (E) |
| sky gradient | 384 rows x 1,440 B = 552,960 B at 1,279.95 MB/s (M) | 0.43 |
| raster: stores | ~2.2 MB incl. ~25% overdraw at 1,279.95 MB/s (M) | 1.7 |
| raster: span + tri overhead | ~48k spans x ~25 cyc + 2,048 x ~150 cyc | 3.8 (E) |
| HUD (digits + attract banner via sprite path) | 96x96 blit = 0.30 ms (M, 3,322/s); ~10 chunk-blits worst | 0.5-2.0 |
| **total** | | **~8.5 typ, <= 12 worst** |

Hard cap: render+HUD <= 18 ms measured, leaving 7 ms for camera/detect/NPU dispatch. On HE (dev loop) multiply by 2.5 (M): ~21-30 ms -> 20 fps (every second vsync) — correct, just slow; HP = HE/2.5 holds because this renderer has no SRAM0 reads (the only path that scaled 1.4x).

## 3. Core allocation

**(a) HP alone — the plan.** 40 Hz with 2x margin, zero new bring-up, bench-proven ATOC boot. Leave it when measured `tr_cyc_render+tr_cyc_hud` > 18 ms mean or `tr_frame_overrun_count` > 0 over a 10 s window at the target poly budget.

**(b) HP + HE.** HE's right job is I/O + vision (camera capture, `tr_detect_frame`, IMU, MHU-1 doorbell to HP). Raster split by bands: HE = 160/(400+160) = 29% of rows -> +40% ceiling, costing a display-list share (36 KB in SRAM0, read at 29 MB/s on HE = 1.2 ms) plus a per-frame barrier. Trigger only if (a)'s rule fires. Engine-vs-render split between the M55s: no — the engine is 0.3-0.5 ms.

**(c) A32 x2.** Cost: bare-metal bring-up (BL33 under TF-A), `apss-mhu` never driven, cross-architecture coherency on the shared back buffer (A32 has L1/L2; HP dcache OFF; the CDC reads SRAM directly — the A32 maps the framebuffer Normal-non-cacheable or cleans ~1.84 MB per frame). **"Engine on one A32, rendering on the other": rejected** — the engine is ~2% of one A32; the split adds a per-frame state handoff for no throughput gain. If the A32s are used, both rasterise (band halves, shared L2 keeps the display list hot) and HP keeps engine + display + I/O + doorbell. Becomes the upgrade if the demo wants Gouraud/textures/5k+ tris.

## 4. The one A32 measurement

Port `probe/fillrate` phases 1b and 3 to an A32 bare-metal image: NEON 128-bit fill into SRAM0 at `0x02000000`, 1 MiB working set, with the CDC200 scanning out live, for (i) Normal non-cacheable and (ii) Normal write-back + `DCCMVAC` clean (clean cost reported separately); plus reads (the M55's weak side, 43 MB/s on HP). Plus one `apss-mhu` doorbell round trip to HP. **Justifies the A32 renderer only if** sustained fill >= ~1,900 MB/s (1.5x HP's 1,279.95) under scanout, or reads are dramatically better, AND the doorbell propagates.

## 5. Implementation plan — ordered, independently testable

All new logic is pure C under `src/render/r3d_*.c` (host-compiled); `render.c` and `display.c` stay thin Zephyr glue. Helium lives in exactly one header.

**T1 — display.c: full-repaint contract.** Remove the dirty-rect list and copy-back (`TR_DIRTY_MAX`, `copy_rect`, `g_dirty*`, `tr_flip_idle_count`); remove the per-row `sys_cache_data_flush_range` (dcache off; pin `CONFIG_DCACHE=n` in `prj.conf` as `probe/fillrate/prj.conf` does). Add `uint16_t *tr_display_back(void);` and `uint32_t tr_display_stride_px(void);`. `tr_display_blit()` keeps working for HUD. `tr_display_flip()` = swap + wait; add `volatile uint32_t tr_frame_overrun_count` (++ when consecutive landed flips are > 1.5 x 25.0 ms apart). Bench check on HE RAM-run: Helium clear + flip loop -> 40.0 flips/s.

**T2 — prj.conf.** `CONFIG_FPU=y`, `CONFIG_SPEED_OPTIMIZATIONS=y`, `CONFIG_DCACHE=n`, `CONFIG_CBPRINTF_FULL_INTEGRAL=y`. `arm_mve.h` needs FPU=y; MVE intrinsics take `(void *)` casts as in the probe.

**T3 — `src/render/span.h`** (the only Helium file):
```c
static inline void tr_span_fill(uint16_t *dst, uint32_t n, uint16_t c);
```
`#if defined(__ARM_FEATURE_MVE) && (__ARM_FEATURE_MVE & 1)`: `uint16x8_t v = vdupq_n_u16(c)`; 8-px loop `vst1q_u16((void *)dst, v)`; tail `mve_pred16_t p = vctp16q(n); vstrhq_p_u16((void *)dst, v, p)`. `#else` scalar loop. Boot self-check in render.c: fill a 64-px DTCM row with lengths 0..17 and compare against a scalar reference, printk `span self-check: OK` once (proves tail predication on silicon).

**T4 — `src/render/r3d.h` / `r3d_math.c` / `r3d_raster.c`** (pure, host-compiled):
```c
#define TR_R3D_W 720
#define TR_R3D_H 1280
#define TR_R3D_SUB 4                      /* 28.4 screen coords */
#define TR_R3D_GUARD (16384 << TR_R3D_SUB)
#define TR_DL_MAX_TRIS 2048
#define TR_DL_MAX_VERTS 1536
#define TR_CAM_Z_NEAR 32.0f
typedef struct { float x, y, z; } tr_v3_t;              /* x right, y up, z = depth (proj.c's z) */
typedef struct { float m[3][4]; } tr_m34_t;
typedef struct { tr_m34_t view; float f_px, cx, cy; } tr_cam_t;
typedef struct { int32_t x, y; } tr_sv_t;               /* 28.4 */
typedef struct { tr_sv_t v[3]; uint16_t c; } tr_tri_t;
typedef struct { tr_tri_t tri[TR_DL_MAX_TRIS]; uint16_t n; } tr_dl_t;
typedef struct { const int16_t *v; const uint8_t *tri; const int8_t *n; const uint8_t *col; uint16_t nv, nt; } tr_mesh_t;
typedef struct { const tr_mesh_t *mesh; tr_v3_t pos; float yaw; float scale_y; } tr_inst_t;
typedef struct { tr_v3_t dir; float ambient; uint8_t fog[3]; float fog_start, fog_end; } tr_light_t;

void     tr_cam_build(tr_cam_t *c, tr_v3_t eye, float yaw, float pitch, float roll, float f_px);
bool     tr_r3d_project(const tr_cam_t *c, tr_v3_t w, tr_sv_t *out, float *view_z);
uint16_t tr_r3d_emit_mesh(tr_dl_t *dl, const tr_cam_t *c, const tr_light_t *l, const tr_inst_t *in);
uint16_t tr_r3d_emit_quad(tr_dl_t *dl, const tr_cam_t *c, const tr_v3_t q[4], uint16_t rgb565); /* pre-lit tiles */
void     tr_r3d_sky(uint16_t *fb, uint32_t stride_px, int32_t horizon_y, uint16_t top, uint16_t bot);
void     tr_raster_tri(uint16_t *fb, uint32_t stride_px, const tr_tri_t *t);
void     tr_r3d_draw(uint16_t *fb, uint32_t stride_px, const tr_dl_t *dl);
```
`emit_*` do backface cull, near clip, fog+Lambert -> RGB565, guard-band clamp, and refuse past `TR_DL_MAX_TRIS` (return 0, count `tr_dl_dropped`). Host tests `tests/host/test_r3d_math.c` (known-point projection, near clip produces 1/2 tris, guard band), `test_r3d_raster.c` (two tris of a quad cover every interior pixel exactly once — top-left rule; clip at all four edges; zero-area and off-screen tris write nothing; scalar `tr_span_fill` == reference for n 0..40 and odd alignment).

**T5 — meshes.** `tools/genmesh.py` -> `src/render/meshes.h` (`static const` tables, ~0.5 KB/mesh, ~6 KB total): `tr_mesh_cap`, `tr_mesh_dip`, `tr_mesh_resistor` (low), `tr_mesh_arch` (high), `tr_mesh_via` (pickup), `tr_mesh_runner_{run0..3,jump,duck}`, `tr_mesh_skyline`. Emits `TR_MESH_MAX_TRIS`. Host test `test_meshes.c`: every index < nv, normals |n| in [110,127], per-mesh nt <= TR_MESH_MAX_TRIS.

**T6 — `src/render/r3d_scene.c`** (pure):
```c
#define TR_PARTICLES 32
#define TR_TILE_LEN 384
#define TR_TILES 24                        /* 24 x 384 = 9216 = TR_PROJ_Z_FAR */
#define TR_CAM_EYE_H 260.0f                /* ponytail: calibration knobs, tune on glass */
#define TR_CAM_BACK 420.0f
#define TR_CAM_PITCH_DEG -14.0f
#define TR_CAM_F_PX 560.0f
#define TR_CAM_BANK_DEG 5.0f
typedef struct { tr_v3_t pos, vel; uint8_t life; } tr_particle_t;
typedef struct { tr_particle_t p[TR_PARTICLES]; uint32_t rng; float cam_x, cam_roll, cam_bob, cam_lift; uint32_t prev_score; } tr_scene_t;
void tr_scene_init(tr_scene_t *s);
void tr_scene_step(tr_scene_t *s, const tr_game_t *g);                 /* easing, bob, particles */
void tr_scene_build(const tr_scene_t *s, const tr_game_t *g, tr_cam_t *cam, tr_dl_t *dl);
```
World mapping reuses `tr_proj_depth_of_model_y()` and `TR_PROJ_LANE_W` from proj.c (its screen-space functions and tests are deleted in T9). Scroll phase for tiles: `(g->tick * TR_SCROLL_PX) % TR_TILE_LEN`. Host test `test_r3d_scene.c`: a known `tr_game_t` -> tri count within budget, runner tris last, every entity emits, ordering monotonic in slice, and a **golden full-frame CRC32** of `tr_r3d_draw` into a host-malloc'd 720x1280 buffer (env `TR_DUMP=1` writes `/tmp/tr-frame.ppm` so the picture is eyeballed on the host before any bench time).

**T7 — render.c rewrite** (keeps `tr_render_init/tr_render_frame/tr_render_banner` signatures). `tr_render_frame`: `tr_scene_step` -> `tr_scene_build` -> `tr_r3d_sky` -> `tr_r3d_draw(tr_display_back(), tr_display_stride_px(), &dl)` -> HUD digits via existing `paint()`/`tr_display_blit`. `tr_render_init` no longer paints. Delete `g_prev_*`, erase pass, `paint_lane_markers`, `lane_x`, `TR_RUNNER_AIR_LIFT`. `tr_dl_t` and `tr_scene_t` file-scope statics (36 KB + ~1 KB -> DTCM). Add `volatile uint32_t tr_cyc_xform, tr_cyc_raster, tr_cyc_hud, tr_cyc_render_max, tr_dl_tris_last, tr_dl_tris_max`.

**T8 — atlas cleanup / ITCM.** Drop runner/obstacle/pickup sprites from `tools/genart.py`+`mkatlas.py` (keep digits + banners); `TR_ATLAS_BYTES` shrinks ~43 KB (E) against ~15-20 KB of new code — net ITCM goes down from 231,520 B. Verify with `nm --size-sort` and the 262,144 B ITCM cap. `runner.sh`: add `src/render/r3d_math.c src/render/r3d_raster.c src/render/r3d_scene.c`. Check `nm` on the ELF that every r3d symbol is linked.

**T9 — loop timing.** `state.h`: `#define TR_TICK_HZ 40`, `TR_SCROLL_PX_PER_S 560` -> `TR_SCROLL_PX 14`, `TR_AIR_MS 450` -> `TR_AIR_TICKS 18`, `TR_DUCK_MS 400` -> `TR_DUCK_TICKS 16`; `attract.h`: 120 / 11 / 53; `main.c`: delete `TICK_MS` and the `k_msleep` pacing (the flip's blanking wait is the clock), `TR_CALIB_TIMEOUT_TICKS 600`; `render.c`: run/pickup cycle 5; `track.h` lost-limit x4/3. Host tests that hard-code tick counts derive from `TR_TICK_HZ`. Delete proj.c's screen-space functions + their tests.

**T10 — camera-build back buffer (booth-critical, off the render path).** Two frames + 512 KiB pool overshoot SRAM0 by 16 KiB. Try first: SE memory-only power request for SRAM1, then read `0x02400000` over SWD and in-image; if it answers, back buffer -> SRAM1. Fallback: crop OV9281 capture to 640x384. Camera is on 2026W36-0001, display on 2026W36-0009 — vision + 3D cannot be validated end-to-end on one board until hardware moves (power off before touching the FFC).

**Bench checkpoints**
- CP1 (host): `tests/host/runner.sh` green incl. goldens; PPM eyeballed.
- CP2 (HE RAM-run, display shield only, attract mode, three 10 s windows over AHB-AP): `tr_flip_count`/s, `tr_cyc_render` mean/max, `tr_cyc_xform/raster/hud`, `tr_dl_tris_max`, `tr_frame_overrun_count`. Expect ~20 flips/s, render 21-30 ms.
- CP3 (HP, authorised ATOC flash, slot0 restored after; counters at HP-local addresses over AP 0x00200000): **pass = 40.0 +-0.1 flips/s, overrun 0, render+HUD <= 12 ms mean, <= 18 ms max**, photo of the panel. Fail -> tune poly budget first, then (b).
- CP4 (optional, T10 done): camera build, vision mode, same counters.

**What would change the recommendation:** CP2 span overhead > 2x the estimate (HE raster > 45 ms) -> banded 360x640 DTCM raster + 2x Helium upscale before a second core; a demand for Gouraud/textures -> same banded path, then the A32.
