# Trace Runner visual polish + crash feel — plan (2026-09-23)

Maintainer feedback on 2026W36-0009 (E1M-AEN803 2026W36-0009), 2026-09-23: "no flicker, it looks
perfect but there can be some improvements for the whole 3D graphics". Asked for, verbatim:
depth fog + sky gradient, more detail (tris/textures), blob shadows + speed lines, lighting
upgrade; "also character can be better, we should have more complex traces. and when crashing
into an object we should see it, now it passes through like a ghost. and the attract mode
animation is a bit fast". Then: "you can use dmas, opcodes, and all the internals to improve
the performance."

Spec: docs/GOAL-3d-game.md + docs/superpowers/plans/2026-09-22-real-3d-renderer.md (3D design) +
docs/superpowers/plans/2026-09-22-a32-renderer.md (A32 architecture). This plan argues from them.

## Global constraints (every task)

- 40.0 Hz vsync-locked on EVERY playing frame stays the bar: core-0 frame must stay <= ~23 ms
  at the default LOD, proven on silicon by the HE flip histogram (all intervals <=27.5 ms).
- Measure before you build. Every perf claim = a number from silicon (stats block 0x02401900,
  tr_prof_t profile build) or, for relative host comparisons, `host ns/frame` from
  test_r3d_scene with -O2, labelled as host.
- Host tests: `bash tests/host/runner.sh` green (incl. A32 qemu cross stage). Goldens change
  only deliberately: regenerate, state old -> new CRC in the commit.
- -ffp-contract=off everywhere; in-repo tr_sincosf; no 64-bit division in per-row/per-pixel loops.
- A new test must fail against broken code first.
- One git worktree per concurrent agent; commit, never push; never touch the bench.
- Bench is main-session only (labgrid 2026W36-0009; 2026W36-0001 OFF-LIMITS). Renderer iterations go
  over the dev loop (LAUNCH into SRAM 0x02500000), not an MRAM write.
- Never SETOTP BBh / SETID C3h to the panel.

## Current cost (silicon, round 3, LOD 0)

core-0 frame 21.5-22.8 ms of 25; ~1,500 tris. Per core: raster ~18 ms (textured spans
~26 cyc/px over ~272k px; per-row/tri overhead ~134 cyc/row x ~30k rows ~4.95 ms; Gouraud
~27 cyc/px); scene build 1.71; setup+bin 1.51; copy 0.76.

## Tasks

### P1 — perf headroom (FIRST; everything below spends it)

Goal: free >= 6 ms on core-0 frame at LOD 0 without changing the picture beyond goldens.
Spike, then build the winners, in this order of expected value:
1. Ground as a background layer: the ground (y=0) never occludes anything above it, so its
   spans skip z-test AND z-write (new TR_TRI_NOZ flag; bands clear z to far). Merge the near
   textured tiles into one long quad per column with UV wrap (texture mask) — ~7 quads instead
   of 7 x tiles, cutting per-row setup overhead.
2. M55-HP as a third raster core: it idles in the A32 design. It boots via the HP ATOC entry
   (flash-jlink-hp.sh evidence; memory note), Helium fill 1279.95 MB/s. Measure its band raster
   cost with the same r3d_raster.c (Helium span.h path) before committing to it; the atomic
   band counter must work across A32 <-> M55 (SRAM, non-cacheable or explicit maintenance;
   LDREX/STREX across masters is NOT proven — use per-core static band ownership if not).
3. PL330 DMA (dma_opcode.h in <Alif Ensemble DFP>) for band copy-out (0.76 ms) and z/colour
   clears. Only if it measurably beats the NEON path.
4. D/AVE 2D (GPU2D): full C driver source IS public — github.com/alifsemi/alif_dave2d-driver
   (d2/ d1/ d0/ + Zephyr module CONFIG_DAVE2D, DT tes,dave2d), pinned in sdk-alif west.yml rev
   d667a8e47f13e2f39bc724d66def5230827b2808; Alif SLA licence (Alif silicon only; fetch as external
   module, never vendor into public alp-sdk). E8: base 0x49040000, clock CLKCTL_PER_MST
   PERIPH_CLK_ENA bit 8 GPU_CKEN (0x4903F00C), IRQ 332 (M55) / GIC SPI 321 (A32), 400 MHz, ~1 px/clk.
   No z-buffer; affine textures; d2_utility_perspectivewarp = per-row mapping = exact for a flat
   ground. NEVER run on an AEN board yet (the old bench "gpu2d PASS" was the SW fallback).
   Top-ranked idea (survey, estimate ~4.4 ms/core, UNMEASURED): CDC200 L1 = sky + textured ground
   drawn by GPU2D from the HE; L2 = A32 foreground, colour-keyed (valid because the ground never
   occludes). Quick subset without GPU2D: sky on L1 (8-bit CLUT), 3D on L2 with CKEY — register
   writes only. CDC200: 2 layers (L1 0x49031100, L2 0x49031200), per-layer alpha, 24-bit colour
   key, windowing, pitch. DMA: all PL330; only DMA0 (NS frame 0x490A0000) reachable from the A32,
   needs DMA_CTRL.BOOT_MANAGER (0x4903F070 bit 0) — unverified; band copy-out saves <=0.5 ms.
   Survey: local work directory accel-survey.md. First profile how much raster time is sky+ground.
Deliverable: numbers table in docs/2026-09-22-measurements.md, chosen items merged.

### P1b — ARM libraries (maintainer question 2026-09-23, measure first)

"do you also use math libraries by ARM with improvements?" Today: hand NEON spans (span.h,
r3d_raster.c), in-repo tr_sincosf (bit-exact host==A32 goldens), newlib memcpy/memset, libm
lroundf. Spike on silicon: (a) CMSIS-DSP NEON (<cmsis-dsp module>, ARM_MATH_NEON)
or Ne10 batched mat-vec for the vertex transform + lighting (scene build ~1.7 ms/core) —
must stay bit-exact host vs A32 (IEEE single, no FMA; NEON FZ only differs on denormals) or
the goldens move deliberately; (b) Arm Optimized Routines NEON memcpy/memset for band clear +
copy-out (~0.8 ms) vs PL330 DMA0. Adopt only what the prof block shows winning.

### P2 — sky, fog, depth

Dithered sky gradient (ordered 4x4 dither, kills RGB565 banding), stars fading in near the
top, sun halo; fog extended to the near textured ground (per-row fog factor in the ground
fast path) so the handover at TEX_Z stays seamless; stronger depth fade on walls.

### P3 — character

New runner mesh (tools/genmesh.py): rounded robot, head with glowing visor, shoulders, elbows,
knees, hands/feet; 8-pose run cycle (was 4) + jump/duck/crash poses; specular on visor/torso.
Budget: <= 2x current runner tris; LOD near/far.

### P3b — skeletal run animation (maintainer 2026-09-23, after seeing the r2 runner on glass)

"the running doesnt look very good. it is okay but it can be better." Cause: 8 baked poses stepped
per ~30 Hz game tick on a 40 Hz panel, no flight phase, little secondary motion. Fix: bind-pose
mesh + ~15-bone rigid skinning evaluated per frame on the A32 from continuous time (HE publishes
render-time sub-tick phase always); parametric run cycle with flight phase, knee drive, heel kick,
planted stance foot (no skating vs scroll speed), hip bob, torso counter-rotation, arm swing, lean;
blended transitions for jump/duck/lane change/crash. Skinning < 0.3 ms/core.

### P3d — planted feet + frame-rate independent easing (maintainer 2026-09-23, on the 30 Hz build)

"the character feels like sliding a bit". Causes: planted feet swept at 31.3 units a tick against
the board's 89.3 (x2.9); and at 30 Hz the renderer's per-frame easing ran at 0.75x real speed.
As built: (1) foot lock -- over its stance the ball of the shoe is pinned to the board point it
touched down on (TR_ANIM_TD_Z - TR_ANIM_GROUND_V x cycle ticks x phase: the board's own travel),
the leg solved by a 2-bone IK with the splay (r3d_rig.c tr_rig_foot_lock, mirrored by genmesh.py
leg_ik); the swing follows a designed foot loop (heel kick, knee drive, reach, paw back), the
thighs counter-yaw the pelvis. A stance can only sweep a leg length (~80 units = 45 ms of board),
so the cycle is 6 ticks (6.7 steps/s at the 20 steps/s play pace, was 4), 15 % stance a foot,
both feet off the board 62.5 % of the cycle (the lock's ease keeps a foot down a little longer
than the 15 %), a 9-unit flight hop. Residual (test_r3d_scene 0f): mid-stance 0.04 units/tick,
worst 0.51 at touch-down / lift-off (was 68.2). (2) tr_frame_in_t.hz (old pad3; 0 = 40): lane
slides, pose blends, landing dip and particles integrate over 40 / hz 40 Hz frames, a packet
change taken as one 40 Hz frame old at any rate; particles ballistic-exact; at 30 Hz the crash
time carries its fraction of a 40 Hz frame (crash_frac, the old pad) so the knock-back steps evenly. 30 vs 40 Hz agree at
equal real times (test_r3d_scene 7b), 40 Hz is bit for bit the old easing. Goldens: rig
0x8093c603 -> 0x9a0c8ad8, scene 0x8e68b7ad -> 0x373bf008.

### P4 — complex traces

Lane + board textures regenerated with realistic PCB routing: 45-degree bends, parallel buses,
differential pairs, vias with annular rings, SMD pads, silkscreen text/outlines. 3D detail on
the shoulders: QFP/BGA packages with legs, SOT/0603 parts, raised copper segments. Stay inside
TR_BIN_MAX and the frame budget (LODs).

### P4b — live-wire obstacles (maintainer idea, 2026-09-23)

"should there be open wires with sparks and arcs as obstacles as well?" New obstacle kind:
open wire between two posts. High variant (chest height, sagging, arcing between the posts)
= duck; low variant (lying on the track, sparking and writhing) = jump. Animated arc: jagged
polyline of thin bright quads (cyan/white), re-randomised every frame from a deterministic
per-frame seed; spark particle bursts where it touches ground/posts. Hitting a wire: electric
crash variant (blue-white flash instead of red, runner jolt). Game: spawn mix includes wires;
packet carries the kind; host tests for spawn/collision/determinism.

### P5 — lighting + shadows + speed lines

Per-vertex Blinn specular on caps/runner; emissive vias that tint nearby ground (coloured
point light, per-vertex); blob shadow under runner and entities (darken-blend ellipse quad,
new TR_TRI_SHADE flag: halve colour in band, no z write); speed lines (thin streaks near the
screen edges, length scales with speed).

### P6 — crash you can see (game logic + scene)

Today the obstacle is judged at the runner line and the frozen frame shows it overlapping
the runner ("ghost"), then a 1.5 s hold. New: on the fatal tick the obstacle stops touching
the runner's front (not overlapping); a ~1.5 s crash sequence keeps rendering frames: runner
knock-back + tumble pose, obstacle shatter/spark particles, camera shake, red flash fading,
slow-motion first 0.3 s. Protocol: TR_FLAG_CRASH + crash tick counter in tr_frame_in_t; the HE
keeps publishing frames during the hold (ui_hold must not freeze the A32 stream). Host tests:
step.c crash state, scene crash pose/particles deterministic.

### P7 — attract pacing

Attract world speed 0.7x of play (sub-tick interpolation: frame packet carries a fractional
scroll so motion stays smooth), calmer wander (TR_ATTRACT_WANDER_IN up), same for jump/duck
cadence. Play speed unchanged.

### P9 — HUD, points, Alp Lab logo (maintainer 2026-09-23)

"we should also add alp lab logo somewhere, also add points and so on." Logo source:
<alplab-logo-white.svg> (paths, fill #e8e8e8; rasterise at
build time, no new runtime deps). Preferred path (measure first): draw the HUD on CDC200 layer 2
(0x49031200: window position, per-pixel/constant alpha or colour key) from the M55-HE, redrawn
only when a value changes — zero A32 raster cost; fall back to a screen-space textured overlay
in the A32 bands if L2 cannot be brought up. Contents: score (points), distance, pickup combo
multiplier, session best; floating "+10"/combo popups at pickups; Alp Lab logo in a corner
during play and large on the attract screen, with a one-line tagline for SoM customers
(E1M-AEN803 · Alif Ensemble E8 · 2x Cortex-A32 rendering 3D at 40 fps) and TILT TO PLAY.
Scoring rules in src/game (host-tested): distance points, pickups +10 x combo, combo resets on
miss/crash.
Perf panel in one corner (maintainer: "also FPS on one corner", "and memory usage", "CPU usage"):
measured FPS (landed flips/s), CPU busy % per core (A32#0/#1 from out_ticks vs frame interval,
M55-HE from tr_cyc_work, M55-HP idle), memory used/total (SRAM0/SRAM1 from a static layout table
checked against the linker maps, HE RAM, MRAM image size); refresh ~2 Hz.

**P9 as built (2026-09-23; L2 up on 2026W36-0009, glass review pending).** Scoring: `src/game/score.c`
(tr_game_t.ev events TR_EV_PICKUP/MISS/CRASH; `tr_game_t.score` stays the raw tally the replay
test and the A32 scene read). HUD: `src/hud/hud.c` paints a 720 x 352 ARGB4444 buffer at SRAM0
`0x02382000..0x023FDBFF` (`TR_HUD_FB`, tr_mbox.h) in 7 fixed tiles, repainting only tiles whose
key changed, composed in a 16-row DTCM strip then copied, capped at 110,000 px a frame; single
buffered (leads the 3D by one refresh; a tile copied across the scan line tears for one
refresh). Fonts (DejaVu Sans Bold 14/18/34/60 px) and the logo are 4-bit alpha maps from
`tools/genhud.py` (pure Python TTF + SVG rasteriser, 4x4 SS). Layer 2 is programmed once by
`src/platform/hud_l2.c` (DFP `cdc_set_layer_cfg()` order: REL_CTRL SH_MASK, window from BP_CFG,
format 7, const alpha 255, blend F1 6 / F2 7, CFB addr/length/lines, CTRL LAYER_EN, REL_CTRL |=
SH_VBLANK) and counts as up only if the reload lands and CTRL/CFB_ADDR read back; then frames
carry TR_FLAG_HUD_L2 and the A32 skips its sprite HUD. Perf panel every 500 ms: FPS from landed
flips; A32 core busy from out_ticks0/1 (core 0 = take-to-publish latency, landed frames);
M55-HP from the P10 sound ring's hp_state ("--" without one); SRAM from the allocation map in
`src/ipc/tr_memmap.h`, the same constants the renderer places its buffers with, + the renderer's
real `__bss_end`; IMG = HE + renderer images. Departures from the spec above: HE busy is the
kernel's runtime stats (non-idle / all cycles), not tr_cyc_work (which includes the flip wait
and reads ~100 % in A32 mode); the SRAM table is tied to the renderer by shared constants, not
checked against the linker maps. Cost (qemu-arm -cpu cortex-m55 instruction counts): score +
distance repaint 0.53 M (every 4th frame), popup frame 0.46 M, a screen change 2.35 M spread over
3 frames by the cap; silicon tr_hud_cyc_max 8.3 ms (docs/2026-09-22-measurements.md). Bench:
`tr_cdc_fifo_underruns` / `tr_cdc_bus_errs` must stay 0 with L2 on.

### P10 — game sound on the M55-HP (maintainer 2026-09-23)

Maintainer chose: M55-HP = game sound now, camera + Ethos-U55 body control later (needs camera
and display on one board). Sound bench: E1M-AEN803 2026W36-0002 ONLY ("you can use that unit for sound only
on the bench now"; reworked I2S mux, TAS2563 amps 0x4d/0x4e). Evidence by PDM-mic loopback, not
by ear. Engine: portable C synth (host-tested, deterministic): music loop (synthwave bass/arp/
drums, procedural or tracker-style pattern data), SFX: footsteps tick, pickup chime, combo
rising pitch, jump whoosh, crash impact + debris, live-wire arc crackle/hum (P4b), attract
jingle. Event protocol from the HE (game events in shared SRAM, like tr_mbox; or MHUv2 doorbell
which is bench-proven HE<->HP). Output I2S -> TAS2563 (alp-sdk chips/tas2563). HP dev loop: find
how to RAM-run or SE-boot the HP without an MRAM write per build (SERVICE_BOOT_CPU 501 from the
HE is bench-proven); final: HP entry in the release ATOC. 2026W36-0009 audio path (amps, speakers,
I2S mux rework) is unverified — never drive IO13/I2S_SELECT high on an unreworked carrier.

### P11-P15 — "more cool stuff"

**Re-prioritised 2026-09-23 (maintainer):** "we should remove glow lights on the PCB traces, doesnt make sense. instead focus on the character and the scene with different scenes" — P12 trace data pulses + pulsing vias removed (LEDs/fans kept); P13 bloom/trace glow dropped; next = character (P16) + world zones (P15); 30 Hz accepted by the maintainer after judging the foot-lock build ("perfect now").
 (maintainer 2026-09-23, after asking about 30 fps)

Maintainer picked all four: living circuit board, glow + light, shadows + reflection, world zones.
Budget first (measure both, keep 40 Hz unless the maintainer prefers 30):
- P11a 30 Hz panel option: retime CDC200 + DSI for a 30 Hz refresh (same resolution, lower
  pixel clock; timing only — never SETOTP BBh / SETID C3h), game tick 30 Hz, a build switch;
  measure flip histogram and judge by eye. 33.3 ms/frame vs 25.
- P11b GPU2D ground on CDC200 L1 (accel-survey.md option 1, ~6.5 ms/core): only if P11a is
  rejected or not enough.
- P12 living circuit board (cheap): data pulses along traces (animated texture phase / emissive
  quads), blinking SMD LEDs, spinning fans on packages, pulsing vias.
- P13 glow + light: bloom on bright pixels (band-local blur of emissive mask), coloured point
  lights from sparks/vias/arcs on nearby vertices, Blinn specular on chips + robot.
- P14 shadows + reflection: blob/projected soft shadows (runner + parts), then planar reflection
  of the runner in a glossy lane (second skinned draw, clipped to the lane) if budget allows.
- P15 world zones: CPU die city, memory-bank canyon, antenna field — per-zone meshes, textures,
  palettes and colour grade, transitions with a gate; HUD shows the zone name.

### P16 — character (maintainer 2026-09-23)

Picked: "Look & detail", "Personality & reactions", "Selectable characters".
- Look: more appealing design — expressive visor/face (emissive eyes that blink/squint/react),
  armour panels with bevels, glowing accents, a trailing scarf/cape (simple verlet chain skinned
  to the chest), a clean colour scheme.
- Personality: glance back at a passed obstacle, fist-pump/spin on pickups and combos, stumble
  on near misses, attract idle/taunt animations (stretch, wave at the passer-by, look at camera).
- Selectable characters: 3-4 characters/skins (distinct silhouettes or palettes on the shared
  rig), chosen on the attract screen by tilting left/right before starting; HUD shows the name.
Keep the planted-foot run, rig tests, contact bound TR_RUNNER_FRONT_Z, y>=0, budget.

### P15 zones (maintainer picks, 2026-09-23): CPU die city, memory canyon, antenna / RF field,
night / neon city — all four, run through in sequence with transitions.

Built (feat/p15-zones), host-proven, not yet on glass:
- Order: the circuit board (zone 0: what an old HE's frames draw, scene golden 0x9c3c1840 unchanged) ->
  CPU die city -> memory canyon -> antenna field -> neon city -> the board. 40 s of play a zone, 16 s in
  attract (a passer-by sees all five in ~80 s); a player's run starts on the board. src/game/zone.h steps
  the schedule with the game; the frame packet carries zone + gate_y behind TR_FLAG_ZONE (164 B).
- Boundary = a gate down the track like an entity (frame + glow in the incoming zone's colours, a threshold
  bar over the ground seam); the look (sky stops, glow = fog, fog palettes, halo, stars, sun / moon,
  skyline) blends over the gate's pass, ~1 s at the play pace. HUD: the zone's name on entry.
- Per zone (tools/genzone.py -> src/render/zones.h): lane + shoulder textures (<= 16 colours, index-only
  at 4 bpp -- integration with P16: the image budget -- unpacked per bound slot into SRAM0 0x022E8000,
  64 KiB; the camera-side pair expanded to RGB565 in SRAM0 0x022D8000, 64 KiB, both outside the
  renderer's 512 KiB),
  a mesh palette with living lights (blinking aviation lights / beacons, DIMM activity LEDs, humming neon),
  shoulder parts with LODs, RF wave rings, neon edge strips.
- Cost, host estimate at silicon unit rates (test_r3d_zones: TR_PROF_NOZ_TEX / GOURAUD+FLAT+NOZ_FILL
  spans / TRI rows / SETUP tris, per-span cost fitted to P12's Gouraud 3.06 ms/core): board +0.00,
  die city +2.77, memory canyon +3.06, antenna field +0.15, neon city +2.83 ms/core; with the gate
  on screen <= +3.01. Silicon check: `make prof`, decode.py --prof per zone (the TRI rows are the
  tall parts' cost).
- Renderer .bin 374,104 B (crc 0x952C17A5) + .bss 49,904: image + .bss end 0x02568870, 428,144 of 524,288 B
  (was 218,232 + 78,628); HE 30 Hz 222,480 B (ITCM 84.9 %).

### P8 — integrate on silicon

LOD fit to 40 Hz at LOD 0; flip histogram all <=27.5 ms; clean SWD capture
(docs/img-2026-09-23-*.png); release ATOC (tilt-ON variant) via Flow D, fresh-session
read-back after a DPS cold cycle; docs updated.

## Order

P1 -> P6 -> P7 -> P2 -> P3 -> P4 -> P4b -> P5 -> P9 -> P8. Serial (P2-P5 all touch r3d_scene.c/genmesh.py;
P6/P7 touch the frame packet). Host PNG preview after each art task for maintainer review.
