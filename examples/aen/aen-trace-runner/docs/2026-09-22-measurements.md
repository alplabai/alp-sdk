# Silicon measurements — 2026-09-22

Unit: E1M-AEN803 2026W36-0009. Core: M55-HE @ 160 MHz, dcache off, RAM-run (ITCM).
Shield set for every game image below: `e1m_evk_rk055hdmipi4ma0` only (no camera).
Panel: 720x1280 RGB565, 40.0 Hz refresh (25.0 ms frame).
Read over the AHB-AP (mem_ap, no halt) from exported counters; 10 s windows timed on the host.

## Double buffer works on glass

Front `lcd_fb` 0x02200000, back `sram0` 0x02000000 (TR_DOUBLE_BUFFER=1).
CDC200 L1 framebuffer register `0x49031134`, 20 samples over ~155 ms:
`0x02000000` x10-11, `0x02200000` x9-10. `g_flip_ok` = 1, CFSR `0x00000000`.
Visual flicker check: NOT yet done by eye.

## Frame rate, attract mode (tr_flip_count, 10 s windows)

| image | build | ITCM used | flips/s |
|---|---|---|---|
| `-Os` (default) | CONFIG_SIZE_OPTIMIZATIONS | 206,696 B | 16.898 / 19.998 / 16.998 |
| speed | CONFIG_SPEED_OPTIMIZATIONS | 230,568 B | 24.698 (incl. start-up) / 19.898 / 18.798 |

No clear difference. `-Os` links picolibc's `space` multilib, whose `memcpy` is a
byte loop (`ldrb`/`strb`); that is what makes `alp_display_blit()` take 244 ms per
full frame, but the game draws straight into the back buffer, so it is not the
game's bottleneck.

## Per-tick cost, speed image (commit 10083b2)

Means per tick (render, work) or per flip (flip_wait, copyback), ms:

| window | ticks/s | work | render | flip_wait | copyback |
|---|---|---|---|---|---|
| W1 | 21.08 | 44.49 | 22.66 | 18.98 | 2.81 |
| W2 | 19.99 | 41.72 | 21.42 | 17.77 | 2.64 |
| W3 | 16.49 | 38.67 | 19.57 | 16.74 | 2.48 |

- `work` = render + flip_wait + copyback (sums agree to 0.1%).
- Render of the current 2D dirty-rect frame costs ~20-23 ms on HE. The swap is then
  applied at the next blanking, so the flip wait averages ~17-19 ms: each tick
  spans ~2 panel frames, which is where ~20 fps comes from.
- Ticks/s falls across windows while per-tick work also falls: the time outside
  `work` is the 1.5 s death pause after each attract-mode run (outside the counters).

## What this says for the 3D renderer

- The present 2D renderer already takes 80-90% of a 25.0 ms panel frame on HE.
- A perspective renderer repaints most of the screen every frame. Full-frame fill
  cost on HE with scalar stores is ~23 ms (~80 MB/s, CPU-bound). The Helium
  128-bit fill number (probe/fillrate phase 1b) decides whether HE can do it at all.
- Moving to M55-HP (400 MHz) is the next lever, but HP is booted only by the SE
  from an ATOC entry (HP-APP `["load","boot"]` at 0x50000000) — that is an MRAM/ATOC
  write and needs explicit authorisation. HP cannot be debugger-loaded on 2026W36-0009
  (`target alif.m55hp examination failed`).

## Fill-rate probe, M55-HE, speed build + FPU (commit 1219346)

| op | MB/s |
|---|---|
| SRAM0 scalar fill (4 KiB - 1 MiB) | 106.53 - 106.63 |
| SRAM0 scalar copy | 29.08 |
| DTCM scalar fill / copy | 106.5 / 106.6 |
| **SRAM0 Helium 128-bit fill, 1 MiB** | **511.82** |
| SRAM0 Helium copy (SRAM0 -> SRAM0) | 65.62 |
| panel framebuffer fill via alp_display path, live / blanked | 34.87 / 34.84 (scanout contention 0) |

Renderer inner loops: 96x96 4bpp->RGB565 sprite 1,328 blits/s; 720-px scanline stretch 31,706 rows/s (45.65 MB/s).

Consequences:
- A full-frame clear (1,843,200 B) with Helium stores is ~3.6 ms on HE. Writes are cheap.
- READS from SRAM0 are the expensive side (copy is 5-8x slower than fill; dcache off).
  So the perspective renderer should repaint from DTCM-resident sources and never read
  the framebuffer back. The current dirty-rect copy-back (2.5-2.8 ms/flip) reads SRAM0
  and becomes unnecessary under a full repaint.
- Scanout costs the CPU nothing measurable.

## M55-HP @ 400 MHz (SE-booted M55_HP ATOC, loadAddress 0x50000000; slot0 restored after)

HP boots only through the SE; not debugger-loadable on this unit. The HP console and counters
read at HP-local addresses over AP `0x00200000` once HP is running (global alias `0x5080xxxx`
did NOT read over that AP).

### Fill-rate probe on HP (same source as the HE run)

| op | HE @160 MHz | HP @400 MHz | ratio |
|---|---|---|---|
| SRAM0 scalar fill | 106.6 | 266.3-266.6 | 2.5x |
| SRAM0 scalar copy | 29.08 | 43.23 | 1.5x |
| DTCM scalar copy | 106.6 | 266.3-266.5 | 2.5x |
| SRAM0 Helium fill | 511.82 | 1279.95 | 2.5x |
| SRAM0 Helium copy | 65.62 | 92.74 | 1.4x |
| sprite 96x96 blits/s | 1,328 | 3,322 | 2.5x |
| scanline rows/s | 31,706 | 79,273-79,279 | 2.5x |
| panel fill via alp_display path | 34.87 | 47.98 | 1.4x |

Compute and stores scale with clock (2.5x). Anything that READS SRAM0 gains only 1.4-1.5x:
SRAM0 read latency is a bus property, not a core one. Full-frame Helium clear on HP: ~1.44 ms.
Scanout contention still 0.

### Game on HP, attract mode (three 10 s windows)

| window | ticks/s | work ms | render ms | flip_wait ms | copyback ms |
|---|---|---|---|---|---|
| 1 | 25.4 | 23.49 | 8.78 | 13.32 | 1.46 |
| 2 | 24.0 | 23.66 | 8.72 | 13.64 | 1.46 |
| 3 | 22.3 | 22.24 | 7.73 | 13.33 | 1.26 |

`work` = render + flip_wait + copyback. Render drops 19.6-22.7 ms (HE) -> 7.7-8.8 ms (HP), 2.5x.
CDC200 `0x49031134` alternates `0x02000000` / `0x02200000`; flip_idle 0.

Frame rate went 17-21 -> 22-25 flips/s, not 2.5x: the loop is now paced by TICK_MS (33 ms)
beating against the 25.0 ms panel frame, not by render. Render at ~9 ms leaves ~16 ms of a
40 Hz frame for the perspective renderer on one core.

## Cortex-A32 bare metal (BL33 under TF-A), E1M-AEN803 2026W36-0009

Probe: trace-runner feat/a32-probe a8e39aa, `a32/probe/a32_probe.bin` (2,488 B) in the A32_APP slot
(MRAM 0x80020000) of the proven A32 ATOC; TF-A sp_min erets to it in NS SVC. Single core (A32_0),
MMU on, L1/L2 on, NEON on. Flow D write + cold-cycle persistence proof, then canonical slot0
(person_detect, md5 5839e003d5d069f6fd912eb22774d037) restored byte-exact.
Results block 0x023FF000 read with no core involved; two dumps 5 s apart identical.

```
magic=0xA32A0001 (OK)  stage=0xD0E  fault=none
CNTFRQ=100000000 SCTLR=0x00C5183D CPACR=0x00F00000 FPEXC=0x40000000 sram1=0xBA7B711B
checksum=0xD489645A  OK
sentinels NC / WB-1MiB / NC-dst: 0xC3A5C3A5 PASS x3
NC       scalar-fill 533.32  neon-fill 1859.49  neon-read 96.81  neon-copy 92.57 MB/s
WB-1MiB  scalar-fill 533.33  neon-fill 1966.65  neon-read 1993.64  neon-copy 1457.83  clean 4544.63 MB/s
WB-16KiB scalar-fill 533.05  neon-fill 1960.29  neon-read 2126.39  neon-copy 1526.37  clean 2773.42 MB/s
```

### A32 vs M55-HP (SRAM0, one core each)

| op | HP @400 MHz, dcache off | A32 | ratio |
|---|---|---|---|
| vector fill, uncached | 1279.95 (Helium) | 1859.49 (NEON, NC) | 1.45x |
| vector fill, cached WB | n/a (dcache hangs) | 1966.65 | 1.54x |
| read | 43.23 scalar / ~93 Helium copy | 1993.64 (NEON, WB) | **~21-46x** |
| copy | 92.74 (Helium) | 1457.83 (WB -> NC dst) | **15.7x** |
| clean 1 MiB to memory | n/a | 4544.63 MB/s (1.84 MB frame ~0.41 ms) | |

- NC NEON read 96.81 MB/s ~= HP's: uncached SRAM0 reads are a bus limit on every master. The A32's win on reads is its CACHE, not its clock.
- Writes: one A32 fills ~1.45-1.54x one HP. Two A32s rastering bands ~2.9-3.1x one HP (estimate, second core not yet run).
- SRAM1: the A32 read 0x02400000 without faulting (0xBA7B711B). SRAM1 is powered in this boot chain (TF-A's own RW is at 0x027DE000). The M55-only boot leaves it unpowered.
- A32 core clock not measured directly (CNTFRQ is the 100 MHz system counter).

### What it means for the renderer

- Flat-shaded painter's renderer (writes only, the plan's design): HP alone is estimated 8.5-12 ms/frame; A32 gives ~1.5x per core — not needed for that design.
- Anything that READS per pixel — z-buffer, textures, Gouraud with read-back, bilinear upscale — costs 20-45x less on the A32 than on the M55s. That is the A32's case: the "rich 3D" upgrade path (textures, z-buffer, >5k tris) belongs on the A32 pair, with the cached back buffer cleaned once per frame (~0.41 ms).

## A32 probe stage 2 — both cores (trace-runner feat/a32-probe 9c3b0ae, resident in MRAM on 2026W36-0009)

Flow D write of three sector-padded blobs (0x80000000 49,152 B; 0x80020000 16,384 B; 0x80578000 32,768 B), ATOC built in a private SETOOLS copy; fresh-session savebin after a DPS cold cycle byte-identical for all three. The A32 dev image now stays resident on 2026W36-0009 (maintainer-approved); canonical person_detect is NOT resident.

```
PMU: 80000196 cycles / 10000000 ticks = 800.0 MHz
PSCI CPU_ON: ret=0 (SUCCESS)  core1 UP, ready, parked
dual-core rounds completed: 6/6
  [WB-S1 dual-fill  aggregate] 2863.27 MB/s   (per core ~1436)
  [NC    dual-fill  aggregate] 2843.88 MB/s   (per core ~1422)
  [WB-S1 dual-read  aggregate] 3284.46 MB/s   (per core ~1653)
  [WB-S1 dual-clean aggregate] 18442.99 MB/s
LDREX/STREX: final=2000000 (OK)
cross-L1 coherency: seed=0x3EB2DB6F (OK)
CDC200 L1CFB/SRCTRL/POS_STAT = 0 (no display running in this boot; no fault)
mailbox sentinel 0x54524D42 @ 0x02401000 (readable over the debug AP)
```
Single-core rows reproduce stage 1 (NC NEON fill 1859.49, WB read 1988.86, WB copy 1461.83 MB/s).

- A32 clock **measured: 800.0 MHz**.
- Two cores together: ~2.86 GB/s fill, ~3.28 GB/s read — a shared ceiling; per-core fill drops from ~1860 to ~1430 MB/s. Plan the band raster against ~2.8 GB/s aggregate, not 2x single-core.
- Heartbeats advance ~3,000/s on both cores while parked (event-stream WFE loop).
- **HE Flow C (RAM-run of the display game) while both A32 cores stay parked: A32 undisturbed** (heartbeats kept advancing), game ran at ~18.5 flips/s (tr_flip_count 0x26B -> 0x290 in 2 s, read via HE DTCM alias 0x58807490). So the dev loop "A32 resident + HE RAM-run" works.

## CP-A2 — first A32 pixels (resident stub, 2026W36-0009)

- Stub f32b59e (3,264 B) flashed as A32_APP (Flow D, sectors 0x80020000 + ATOC 0x80578000, cold-cycle read-back identical). It relocated itself to SRAM1 0x0240C000 (copy == .bin), mailbox 0x02401000 magic 0x54524D42 v1, stub_state PARKED, core 1 PARKED, heartbeats ~3,000/s on both cores.
- HE RAM-run of the A32-mode M55 image (T-A4, 142,300 B, shield e1m_evk_rk055hdmipi4ma0): display up, "D-cache left ON by a previous image (mailbox is MPU non-cacheable)" logged — the MPU region covered it.
- Colour-bar payload 0ab3c8b (424 B, CRC 0x03075E84) written to 0x02500000 and LAUNCHed over the M55 debug AP (no MRAM write): stub_state RUNNING; FB B (being scanned, CDC200 0x49031134 = 0x02200000) holds 8 A32-written bars 0000 FFFF FFE0 07FF 07E0 F81F F800 001F, 90 px each; FB A bars too. TF-A MHU0 window 0x02380000-0x02380FFF not written.
- Frame handshake not exercised yet (payload does not echo in_seq; HE watchdog logs "no out_seq for 100 ms" as designed). That is CP-A4.
- Visual confirmation by eye: pending.

## CP-A4 — M55<->A32 frame handshake on silicon (2026W36-0009)

Echo payload 549b393 (2,336 B, CRC 0x1437CE34) LAUNCHed over the M55 AP into the resident stub; HE runs the A32-mode image (T-A4, 142,300 B, shield e1m_evk_rk055hdmipi4ma0). Per frame: HE publishes in_seq + free FB -> A32 core 0 NEON-fills that FB with a tick-keyed colour -> out_seq = in_seq -> HE flips at vblank.

Counters over a ~10 s window (read over the AP, HE DTCM alias 0x5880xxxx):

| counter | start | end | delta |
|---|---|---|---|
| tr_flip_count | 0x50 | 0x168 | 280 |
| tr_in_seq_published | 0x82B92401 | 0x82B92519 | 280 |
| A32 out_seq | = in_seq | = in_seq | every frame answered |
| tr_a32_timeouts | 0x8A5 | 0x8A5 | 0 (old count from the colour-bar run) |
| tr_frame_overrun_count | 0 | 0 | 0 |
| tr_flip_dropped | 0 | 0 | 0 |

CDC200 0x49031134 sampled alternating 0x02200000 / 0x02000000. A32 full-frame NEON fill: out_ticks0 0xFD33 = 64,819 ticks = 0.65 ms.

Wall-clock average 28 flips/s includes attract-mode death holds (ui_hold(1500) per run, excluded from overrun accounting). tr_frame_overrun_count = 0 means no landed-flip interval exceeded 37.5 ms outside holds, i.e. every playing frame landed on the next vblank (40.0 Hz). Inferred from the counter semantics, not yet a direct interval histogram — add one at CP-A5.

## CP-A5/CP-A6 (single core) — A32 renderer on silicon, golden DL

Renderer 2947edd (27,272 B .bin at 0x02500000; .text 9,612 / .rodata 15,027 / .bss 37,096) LAUNCHed into the resident stub over the M55 AP; HE runs the A32-mode image. Renders the host-emitted golden DL (287 tris, full-screen sky + ground, textured/Gouraud/flat) through tr_bin_build + 40 x 32-row cached colour/z bands + write-only NEON copy, core 0 only.

| item | value |
|---|---|
| boot self-checks (NEON span, raster vs scalar oracle) | 0x80000003 = both PASS on silicon |
| FB CRC (first frame, every 256th) | 0xAEA2B368 = host golden, 2/2 matched — **bit-exact A32 vs host** |
| frame time out_ticks0 (100 MHz ticks) | 0x144C6A = **13.30 ms**, max 0x14734B = 13.40 ms |
| bin + setup | 0xE4F4 = 0.59 ms |
| raster (bands) | 0x124068 = 11.96 ms (~10.4 cycles/px at 800 MHz over 921,600 px) |
| band -> FB copy | 0x12664 = 0.75 ms |
| frames in ~10 s window | 307 published = 307 flipped (average includes attract death holds) |
| timeouts / overruns in window | +1 / +1 (likely the frame after a CRC read of the uncached FB) |

One A32 core renders the full-screen golden frame in 13.3 ms — inside the 25.0 ms frame, so 40 Hz is available single-core for this content; the second core is headroom for richer scenes.

## CP-A6 — dual-core A32 raster + direct 40 Hz proof (2026W36-0009)

Renderer T-A6 55987b8 (28,328 B, both A32 cores, atomic band counter, per-core cached colour/z bands) + HE image with flip-interval histogram (142,412 B, shield e1m_evk_rk055hdmipi4ma0), golden DL.

| item | single core (cdba53d) | dual core (55987b8) |
|---|---|---|
| frame time core 0 (out_ticks0) | 13.30 ms | **7.19 ms** (max 7.42) — 1.85x |
| core 1 busy (out_ticks1) | – | 6.37 ms |
| bin+setup / raster / copy | 0.59 / 11.96 / 0.75 ms | 0.59 / 6.13 / 0.47 ms |
| bands per core | 40 / – | 20 / 20 |
| self-checks | pass | pass (0x80141407) |
| per-band CRC vs host golden | – | 400 matched / 0 mismatched |

HE flip-interval histogram over the ~10 s window: 339 new intervals, **all in the <=27.5 ms bucket** (40.0 Hz during play; attract death holds excluded by design); bucket >177.5 ms = 2 (pre-window); tr_frame_overrun_count unchanged at 2. **40 Hz vsync-locked is now measured directly, not inferred.**

## Real 3D scene on the A32 pair — first silicon run (2026W36-0009)

Renderer feat/a32-dual 5686386 (scene build default, 170,180 B) LAUNCHed into the resident stub; HE runs the A32-mode image with the flip histogram. Clean capture (renderer HALTed, then SWD dump of the last completed FB 0x02000000): docs/img-2026-09-22-a32-scene-silicon.png — matches the host art (sunset, capacitor/DIP walls, lanes with copper traces, vias, probe robot, "STEP IN TO PLAY" banner, score).

| item | value |
|---|---|
| tris per frame | 1,483 (max band bin 568, drops 0) |
| core 0 frame (out_ticks0) | **31.16 ms** |
| core 1 busy (out_ticks1) | 21.15 ms |
| scene build (core 0 only) | 6.61 ms |
| bin + setup (core 0 only) | 3.40 ms |
| raster per core | 20.6 ms |
| band copy | 0.43 ms |
| HE flip histogram, window | 169 intervals, all in <=52.5 ms → **20 fps** |

Known defects: (1) 31 ms > 25 ms frame → 20 fps; front-end serial on core 0 and textured raster dominate. (2) TR_FB_B overlaps the TF-A MHU0 window (rows 1092-1094 of FB B never written → stale strip every other frame). An earlier live (non-halted) SWD dump showed sheared walls/garbled banner — that was the 9 s dump mixing frames, not a render bug.

## CDC200 scanout from SRAM1 (2026W36-0009)

Test payload fbsram1 (feat/fbb-sram1 d59efb8, 1,844 B) fills 0x02600000..0x027C1FFF (0x027 section mapped via a 4 KiB second-level table so TF-A RW 0x027DE000.. stays unmapped); HE image built with -DTR_FB_B_ADDR=0x02600000u (142,348 B, shield e1m_evk_rk055hdmipi4ma0); cdc200_swap_fb() accepts it (fb_size check only).

- CDC200 0x49031134 sampled alternating 0x02000000 / 0x02600000 — swaps to the SRAM1 buffer land.
- 150 flips in ~5 s answered by the payload; tr_a32_timeouts unchanged.
- CDC200 driver data words at data_0+0x60/+0x64 (bus_err_count / fifo_underrun_count by struct layout): 0x201 / 0 before and after — no new errors while scanning SRAM1 (offsets inferred from the struct; confirm by symbol if it matters).
- Visual confirmation by eye: pending.
Conclusion: FB B can move to SRAM1 0x02600000 in the A32 boot chain, removing the MHU0-window strip.

## Tilt takeover — flat-board check on silicon (2026W36-0009)

HE image feat/tilt-takeover 055c67c (142,820 B, shield e1m_evk_rk055hdmipi4ma0) + scene renderer on both A32 cores. Board untouched (flat, no person):
- tr_bench_mode = 2 (ATTRACT) at t+3 s and t+6 s; tilt engages = 0, walk-aways = 0, gestures = 0 — no false takeover.
- IMU level-zeroed q8 (256 = 1 g): x = -1, y = +3 / +2 — inside the ±15 re-arm band; engage threshold is 90 (~20.6 deg) held 20 ticks.
- Renderer RUNNING (core 1 RUNNING), out_ticks0 this sample 24.4 ms (attract content varies frame to frame).
Not yet proven: an actual tilt taking over (needs a person to tilt the board).

## Renderer optimisation round 1 on silicon (feat/a32-dual ef88de5, FB B in SRAM1)

- FB B moved to SRAM1 0x02600000 (MHU0-window strip gone); CDC200 alternates 0x02000000/0x02600000.
- core 0 frame 23.2-24.5 ms (was 31.16), max 26.8; core 1 20.0-21.2; scene 3.0-3.2 ms (was 6.61); setup+bin 1.42 (was 3.40); raster 17.8-19.4/core (was 20.6); copy 0.76.
- HE flip histogram, 10 s: +72 intervals <=27.5 ms (40 Hz), +133 <=52.5 ms (20 Hz) — frames just miss vsync.
- Profile build, per core per frame: textured spans 8.9 ms @ 26.2 cyc/px (272k px); per-row/tri overhead ~4.95 ms (~134 cyc/row, ~30k rows); Gouraud 3.7 ms @ ~27 cyc/px but ~9 px/span; bg 0.47; setup ~1.3.

## Renderer optimisation round 2 on silicon (feat/a32-dual e0bc37e)

Leaner NEON texture gather, branch-free edge bounds, overlapped span ends, scene build split across both cores (core 1 builds the tail into its own DL, appended in order — image unchanged).
- core 0 frame 21.9-22.3 ms (max 24.6), core 1 21.9-22.3 (balanced); scene 1.71 ms; setup 1.51; raster 18.2; copy 0.76.
- HE flip histogram over ~10 s: **+305 intervals <=27.5 ms (40 Hz), +17 <=52.5 ms** — ~95% of frames at 40 Hz. Remaining misses: frames near 24-25 ms incl. the M55's 1 ms handshake poll.

## Round 3 — 40 Hz on every playing frame (feat/a32-dual 4c77bb8 (agent had reported 06f96c1, which does not exist), HE ta8)

A32 core 0 spins on in_seq (no WFE latency); HE polls out_seq / flip landing every 100 us (was 1 ms); runtime LOD word at 0x02401904.

| | LOD 0 (full detail) | LOD 3 |
|---|---|---|
| core 0 frame | 21.5-22.8 ms (max 24.0) | 18.0 ms |
| tris | ~1,500 | 1,179 |
| HE flip histogram, ~10 s | **+233 <=27.5 ms, 0 <=52.5 ms** | **+339 <=27.5 ms, 0 <=52.5 ms** |

out->in gap (includes the vblank wait): 0.82-2.05 ms.
**Result: the real 3D scene on the two A32 cores holds 40.0 Hz vsync-locked on every playing frame at full detail, measured directly by the landed-flip histogram.** LOD 3 leaves ~7 ms headroom.

## Release image: standalone boot to the 3D game (2026W36-0009, feat/a32-dual eca9923)

Flow D write (A32_APP 0x80020000 = release stub + embedded renderer 181,328 B; ATOC 0x805589D0..0x8057FFFF with HE_APP = trace-runner HE A32-mode AUTOLAUNCH image, loadAddress 0x58000000 load+boot, shield e1m_evk_rk055hdmipi4ma0), sectors merged from a live MRAM read-back; fresh-session read-back after a DPS cold cycle identical for both. Then a cold cycle with NO debugger load:
- HE booted from the ATOC (ITCM 2000DE60 0000954D), console "m55 boot #1", display up.
- Stub magic 0x54524D42 v1; both A32 cores RUNNING the renderer.
- Start-up race: the HE came up before the stub initialised the mailbox → "stub not PARKED ... no LAUNCH", then watchdog HALT #1 / LAUNCH #2 → rendering. Self-recovered (tr_a32_relaunches = 2).
- Over ~10 s: HE flip histogram +306 intervals <=27.5 ms, 0 <=52.5 ms → 40 Hz on every playing frame; out_ticks0 21.9-22.8 ms; CDC200 alternating FB A / FB B (SRAM1).
**The board now boots by itself into the real 3D game at 40 Hz.**

## Release image v2 (watchdog window fix, feat/core 3bd1ef5) — standalone, 2026W36-0009

ATOC re-flashed (HE 144,240 B, shield e1m_evk_rk055hdmipi4ma0; A32_APP unchanged), sector merged from the proven MRAM image, read-back identical after a DPS cold cycle. Cold boot, no debugger load:
- HE boots first (console "stub not PARKED ... no LAUNCH"), IMU rest from 31/32 samples, ATTRACT; watchdog LAUNCH #1 once the stub is up → both A32 cores RUNNING. Start-up takes several seconds (an 8 s-after-power sample still showed an uninitialised mailbox).
- Flip histogram since boot (tr_flip_hist @0x200051b8 in this build — addresses move per build, always nm): 2,325 intervals, **all <=27.5 ms**; one >177.5 ms (start-up); overruns 1 (start-up). Last 10 s: +339 at <=27.5 ms, 0 slower.
- core 0 frame 20.4-22.3 ms.

## Release image, tilt ON (rel/tilt-takeover = feat/core 17f7dc5) — standalone, 2026W36-0009

ATOC re-flashed (HE 144,332 B, AUTOLAUNCH + TR_TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0; A32_APP unchanged, md5 871b292869f3ba86e3ce1271e0227ea9). 163,840 B blob at 0x80558000 (md5 b9e92a8f1651fc8abe3c662b85d8d7a6) merged from the proven v2 MRAM image; A32 confirmed running from SRAM (0x02401184 = 0x02500000) at write time. Fresh-session read-back after a DPS cold cycle: byte-identical. Cold boot, no debugger load, board lying flat:
- tr_bench_mode = 0x02, tr_tilt: playing 0, armed 1 (attract, waiting for a tilt), engages 0, walkaways 0, gestures 0.
- tr_imu_x_q8 / tr_imu_y_q8 = 0 / 0 (rest-zeroed Q8; flat board, below 1/256 g).
- Flip histogram (@0x200051b8): 0x5A9 then 0x60F intervals <=27.5 ms across two reads a few seconds apart; one start-up interval in a slow bin, unchanged between reads.
- Not yet proven: a person tilting the board (engage, lane change, walk-away).
- 2026-09-23 maintainer by eye on the panel: "there is no flicker, it looks perfect".

## P1 ground fast path on silicon (feat/p1-ground 829921c, 2026W36-0009, dev-loop LAUNCH over the tilt-ON release HE)

renderer.bin 184,012 B (crc 0xD9CC36D5) LAUNCHed at 0x02500000; no MRAM write.
- Old (release-embedded) renderer, same session: out_ticks0 0x20B34D..0x22491C = 21.43-22.48 ms.
- P1 renderer: out_ticks0 0x18A642..0x199CA7-ish = 16.15-16.77 ms, core 1 16.15-16.73; ~1,270-1,350 tris.
- HE flip histogram after launch: +340 intervals <=27.5 ms over the ~10 s window, 0 slower (2 slow intervals are the HALT/LAUNCH gap).
- Profile build (renderer-prof.bin 185,100 B, 126 frames), per core per frame: noz_tex 6.35/6.24 ms @ 18.6 cyc/px (272k px; was ~26 cyc/px z-tested); gouraud 2.72/2.60 @ 23.5-24.9 cyc/px; tri total 11.93/11.87 (overhead ~2.7 ms); setup 1.11/1.30; bg 0.47; bin 0.16.
- Headroom: ~8.5 ms of the 25 ms frame. GPU2D ground-on-CDC-L1 would remove a further ~6.3 ms/core (noz_tex) — kept in reserve.
### After fix round (c7f190a = P1 + feat/core P6/P7 merge, board tuck)
renderer.bin 187,660 B (crc 0xCADFA1C2). core-0 frame 0x17BD59..0x193BE1 = 15.58-16.52 ms (core 1 15.50-16.51); ~1,300-1,470 tris. Flip histogram over ~10 s: +339 <=27.5 ms, 0 slower. Prof (188,748 B, 120 frames) per core: noz_tex 6.21/6.08 ms @18.1-18.2 cyc/px, gouraud 2.58/2.48, tri 11.53/11.45, setup 1.11/1.28.

## P3 smooth runner on silicon (feat/p3-runner d173b79 = runner r2 + P1, 2026W36-0009, dev-loop LAUNCH)
renderer.bin 268,008 B (crc 0xB200F109; ~700 tris/pose, body+limbs meshes). core-0 frame 0x190AED..0x1BF270 = 16.41-18.34 ms (P1 alone 15.58-16.52); 1,571-1,733 tris. Flip histogram +339 <=27.5 ms over ~10 s, 0 slower.
Prof (269,096 B, 166 frames) per core: gouraud 2.79/2.68 ms, tri 12.22/12.09, setup 1.39/1.58 (812 tris/core), noz_tex 6.24/6.12, bg 0.47. Split tile 20: parts 862/837 tris (host).

## P2 sky/fog on silicon (feat/p2-sky 0f4f412, before merge with P3)
renderer.bin 192,828 B (crc 0xE2E038F7). core-0 frame 18.34 ms (P1 alone 15.6-16.5); all flips 40 Hz. Prof per core: bg 1.19/1.11 ms (was 0.47), noz_tex 7.35/7.17 @21.5 cyc/px (was 6.21 @18.1), tri 12.83 (was 11.53). Sky+fog ~+2.0 ms/core measured (agent estimate 0.75; budget 2.5).

## P2 + P3 combined on silicon (feat/p2-sky b9c2290 = sky/fog + smooth runner, 2026W36-0009, dev-loop LAUNCH)
renderer.bin 273,180 B (crc 0xE90DF4F6). core-0 frame 0x1C284E..0x1D7EA0 = 18.55-19.34 ms; 1,538-1,658 tris. Flip histogram +229 <=27.5 ms between reads, 0 slower.
Prof (274,332 B, 125 frames) per core: bg 1.17/1.13, noz_tex 7.31/7.19 @21.4 cyc/px, gouraud 2.61/2.56, tri 13.04/12.96, setup 1.38/1.58. Headroom ~5.7 ms.

## P3b skeletal animation on silicon (feat/p3b-anim d37dd3c, 2026W36-0009, dev-loop LAUNCH)
renderer.bin 191,396 B (crc 0x3A51A31E; skinned 21-bone runner, 736 tris). core-0 frame 0x1D15E1..0x1F5792 = 19.07-20.54 ms. Flip histogram +348 <=27.5 ms over ~10 s, 0 slower. Prof per core: tri 13.28/13.18, setup 1.42/1.63, gouraud 2.73/2.62, noz_tex 7.32/7.14, bg 1.19/1.11.
## P9 HUD on CDC200 L2 on silicon (feat/p9-hud bb8180a, 2026W36-0009): HE RAM-run (Flow C, <build-dir>, shield e1m_evk_rk055hdmipi4ma0) over renderer 187,724 B crc 0xD5082CAB
Console: "hud : layer 2 720x352 ARGB4444 @0x02382000 win h 0x02ed001e v 0x016f0010 rel 0x4 (21 ms)". L2 readback 0x49031200..: CTRL(0x20C)=1, HPOS=0x02ED001E, VPOS=0x016F0010, PIX_FORMAT=7, CONST_ALPHA=0xFF, BLEND_CFG=0x00010607, CFB_ADDR=0x02382000, CFB_LENGTH=0x05A005A7, CFB_LINES=0x160; REL_CTRL=4 (SH_VBLANK self-cleared).
tr_hud_cyc_max 0x144D60 = 1,330,528 cyc = 8.3 ms @160 MHz; last 0x769. Flip histogram +200 intervals <=27.5 ms in ~5 s, 0 slower. core-0 frame 0x17A691..0x17E476 = 15.5 ms (older P9-base renderer). Visual on glass: pending maintainer.

## P9 r2 HUD on top of the full new scene (feat/p9-hud ff96b35 = HUD + sky/fog + smooth skinned runner, 2026W36-0009)
HE RAM-run <build-dir> (ITCM 220,232 B, shield e1m_evk_rk055hdmipi4ma0) + renderer 191,652 B crc 0xFA5423F7 (matches the HE autolaunch id). Console "hud : layer 2 720x352 ARGB4444 @0x02382000 ... rel 0x4 (21 ms)"; tr_hud_l2_open succeeded. L2 readback CTRL=1, HPOS 0x02ED001E, VPOS 0x016F0010, CFB 0x02382000, REL_CTRL 4. tr_cdc_fifo_underruns 0, tr_cdc_bus_errs 0 (over ~12 s). tr_hud_cyc_max 0x144D9F = 8.3 ms @160 MHz. Flip histogram +320 <=27.5 ms in ~8 s, 0 slower. core-0 frame 0x1D7121..0x1D76B8 = 19.3 ms.

## P4 traces + P4b live wires on silicon (feat/p4-traces 253289f, 2026W36-0009)
HE RAM-run /tmp/tr-p4-he (144,412 B, shield e1m_evk_rk055hdmipi4ma0; wires spawn 1 in 5 obstacles) + renderer 208,924 B crc 0x0AD20620. core-0 frame 0x1D028A..0x208B89 = 19.02-21.36 ms (1,801-2,097 tris). Flip histogram +400 <=27.5 ms over ~10 s, 0 slower. Prof (210,076 B, 184 frames) per core: setup 1.56/1.85 ms (947 tris/core), tri 12.91/12.79, gouraud 2.56/2.43, noz_tex 7.29/7.19, bg 1.19/1.11.

## Release v3 — everything, standalone from MRAM (feat/core 1b2599a, 2026W36-0009, 2026-09-23)
HE /tmp/tr-rel-he: 221,236 B ITCM, shield e1m_evk_rk055hdmipi4ma0, TR_M55_AUTOLAUNCH + TR_TILT_TAKEOVER, md5 da002d6cffec64922080f66f2e78aebb; no I2S/TAS2563/CC3501E linked (2026W36-0009 carrier stock U46, no speakers). Renderer 208,992 B crc 0xF5C3FD20 (autolaunch id match). Packaged with build-release.sh (private SETOOLS copy) against a live MRAM read-back.
Flow D (A32 in SRAM, 0x02401184 = 0x02500000; HE halted): a32_app 0x80020000 212,992 B (md5 83f5d702420d8ca030985d9926250090) + atoc 0x80544000 245,760 B (md5 6ba51ff093cccfb749bf826a250644ca); bl32 sectors identical to live, not written. Fresh-session read-back after a DPS cold cycle: both byte-identical.
Cold boot, no debugger load: console "hud : layer 2 ... (21 ms)", "imu : READY (tilt mode available)", "mode : ATTRACT"; watchdog start-up relaunch as before; stub RUNNING both cores, ctrl {0x02500000, 0x33060, 0xF5C3FD20}. Flip histogram +400 <=27.5 ms in ~10 s, 0 slower; CDC underruns 0; tilt armed (tr_tilt playing 0 armed 1); core-0 frame 0x1E0099..0x1F82B0 = 19.66-20.65 ms.
Contents: dusk sky + stars + halo + ground fog, PCB-routed traces + shoulder parts, live-wire obstacles, smooth skinned 21-bone runner, visible crash, 0.7x interpolated attract, HUD on CDC200 L2 (score/combo/best/popups/Alp Lab logo/perf panel). Sound: proven on 2026W36-0002 only (not on this board).

## Body control on silicon — tilt takeover by a person (2026W36-0009, 2026-09-23 ~11:15)
Build on the board: P3c HE (feat/p3c-pace 8a58410, TR_TILT_TAKEOVER=ON) RAM-run + renderer crc 0xAA4D8561. The maintainer tilted the board by hand.
- IMU (rest-zeroed Q8, 256 = 1 g) over a 90 s, 5 Hz SWD recording: x -56..+45, y -63..+93.
- tr_tilt (0x5880DCC0) during the recording: playing=1 in 10 of 27 samples (0x00010001), armed/attract otherwise.
- tr_tilt counters afterwards: engages 2, walkaways 2, gestures 33 (0x21), idle_ticks 211; state back to armed attract.
- Maintainer: "tiliting now works but in opposite direction" / "when I tilt left, it goes right" -> steer sign mirrored on this mounting; fixed by TR_TILT_STEER_SIGN (-1) in feat/p3c-pace 10dcef8 (test: x +95 -> lane -1). Pitch confirmed correct: toward = jump, away = duck.
**Result: body control (tilt) works on silicon — a person takes over the attract game by tilting, plays (lane changes, jumps, ducks), and walking away hands back to attract.**

## Release v4 — standalone from MRAM (feat/core 4ab266d = P3c ad297df, 2026W36-0009, 2026-09-23)
HE /tmp/tr-p3c-he 221,368 B md5 8ffe67c9c68e1fa763c26ff856676f9e (A32, TR_M55_AUTOLAUNCH, TR_TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0; no I2S/TAS2563/CC3501E). Renderer 212,856 B crc 0xAF5FDA49 (autolaunch match). Packaged against a live MRAM read-back; bl32 sectors identical (not written).
Flow D (A32 in SRAM 0x02401184 = 0x02500000, HE halted): a32_app 0x80020000 229,376 B (md5 99cb90839d0cbd48b8210b0203d0dbd7) + atoc 0x80544000 245,760 B (md5 9d57e9a3af3bac5baa74b97b431529d6). Fresh read-back after a DPS cold cycle: both byte-identical.
Cold boot, no debugger: HUD L2 up, imu READY, ATTRACT; over 20 s tr_ticks +800 (no hang) and +800 flips all <=27.5 ms, CDC underruns 0, tilt armed; stub ctrl {0x02500000, 0x33F78, 0xAF5FDA49}; core-0 frame 0x1E7364..0x1FCF3D = 19.95-20.87 ms.
Contents over v3: stylised sprint (4 steps/s at 1.0x), game pace 0.5x (TR_GAME_PACE_Q8 128), crash 1.5 s real time, tilt EDGE 48/DEAD 20/ENGAGE 64 + steer sign -1 + pitch needs |y|>|x| + lane on the detecting frame, BMI323 200 Hz, obstacles spawn at TR_SPAWN_Y -440 and emerge through entity fog, camera lower, duck roll, lane hop, speed lines.

## P12 living circuit board on silicon (feat/p12-living e57d90e, 2026W36-0009, 40 Hz, dev-loop LAUNCH)
Renderer 219,376 B crc 0xE411180B (review-fix b066e51 = 0x821BFAB7: LOD gate + tests only). core-0 frame 0x1F499D..0x21810B = 20.52-21.92 ms (release v4 19.95-20.87); flip histogram +400 <=27.5 ms in ~10 s, 0 slower; CDC underruns 0. Prof (13 frames) per core: tri 14.25/14.20 ms (rows 18.7k), gouraud 3.06/3.07 @28 cyc/px, setup 1.85/2.22 (1209 tris/core), noz_tex 7.29/7.27, flat 0.05, bg 1.27. Living board ~+1.0 ms/core. 40 Hz headroom now ~3 ms.

## 30 Hz panel + planted feet + trimmed living board on silicon (feat/p3d-footlock 6523123, 2026W36-0009, RAM-run)
30 Hz (panel_30hz.overlay: vfront-porch 16 -> 454, pclk 40 MHz, lane rate 483.56 Mbps unchanged): HE /tmp/tr-p3d-he30 221,544 B md5 b073f455b4eb3adf3310aec3b5212923 (A32, TR_PANEL_HZ=30, AUTOLAUNCH, TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0) + renderer 219,128 B crc 0xA172A892. Flip histogram +300 in ~10 s, all in the first (<=35.8 ms) bin; CDC underruns 0; core-0 frame 0x1F1A68..0x2165 4F = 19.87-21.91 ms of 33.3 ms. Earlier 30 Hz check (P11a 4234f47): 301 flips/10 s, underruns 0, bus errs 0.
Planted feet (P3d): stance foot vs board residual 0.511 u/tick worst (touch-down/lift-off), 0.04 mid-stance (was 68.2 u/tick worst); run cycle 6 ticks = 6.67 steps/s, 62.5% flight. Maintainer: "perfect now". 30 Hz accepted as the base.

## Side shimmer + speed lines (fix/side-flicker 9f7ebf6, 2026W36-0009, 30 Hz RAM-run)
Maintainer photo showed short white streaks across the tall capacitors at the screen edge = the P3c speed lines (screen-space, drawn in front of everything) -> removed. Separately the shoulder board texture aliased at distance (shimmer 15.9 -> 3.9 per frame at depth 1500-2200 after fade_detail; flicker-pixel share 13.5% -> 5.2%, lanes 4.45% control). HE /tmp/tr-flk-he30 (BOARD alp_e1m_aen803_m55_he, 30 Hz, shield e1m_evk_rk055hdmipi4ma0; an agent first built it for aen801 — rebuilt) + renderer 218,232 B crc 0x22FCB61F: 300 flips/10 s all on time, underruns 0, frame 19.8-20.2 ms.

## P16 characters, personality, selection (feat/p16-character, HOST ONLY -- not yet on silicon)
Renderer 265,352 B crc 0x69108422 after the feat/core merge (6580e15; P16 alone 266,312 B crc 0x2EEC493F; was 219,128 B: +47 KB for the four characters' meshes; the tables are referenced from r3d_rig.c only, so one copy links). HE /tmp/tr-p16-he30 222,992 B md5 a9ac4f0fca1127174905d197532c3e19 (A32, TR_PANEL_HZ=30, AUTOLAUNCH, TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0; autolaunch id {0x02500000, 0x40C88, 0x69108422}; no I2S/TAS2563/CC3501E code, the same empty i2s section markers as P3d).
Tris per character (meshes + 8 eye tris + the scarf's visible side): PROBE 866, SOLDER 850, FLUX 782, PIXEL 824 (budget 900; P3b runner 736). Golden frame 2037 -> 2099 tris (Probe); 2079 after the merge (speed lines gone).
Runner raster work, host coverage over 64 run frames (mean a frame, runner + scarf only): P3d 363 tris / 5,692 rows / 14,760 px; PROBE 426 / 5,911 / 14,971; SOLDER 426 / 6,161 / 16,925 (worst); FLUX 380 / 5,088 / 12,791; PIXEL 407 / 5,510 / 14,126.
Estimate at the P12 silicon rates (setup 1.53 us/tri, ~134 cyc/row, Gouraud ~28 cyc/px, 800 MHz, bands shared by both cores), worst character: +63 tris setup ~0.05 ms/core, +469 rows + 2,165 px (x1.5 overdraw) ~0.10 ms/core, skin + scarf + eyes (host +2 us of 15 us; one rig pose a frame for the scarf anchor) ~0.02 ms -> ~+0.2 ms/core, ~+0.4 worst with a reaction's hold_front pass. Budget +3 ms/core. To confirm on silicon: `make prof` renderer, TR_PROF_SETUP / TR_PROF_TRI / TR_PROF_GOURAUD per core and pad3[5] (scene ticks), against the P3d numbers above.

## P16b character detail ("sharp edges") (2026W36-0009, 30 Hz RAM-run, bca3345)
Diagnosis (close-ups, local work directory p16b/diagnosis.md): mostly faceted silhouettes -- single-apex tube ends (pointed shoes, mitts, torso, pack, Solder's stacks), 6 / 8-sided limbs, 8 x 3 shoulder balls; then low-segment Gouraud; joints were closed (gap 0.003); 1-px outline stair-steps present but secondary (no outline AA this round). Fix: rounded ends, more segments, a weld pass (coincident same-bone vertices < 30 deg apart share one normal; tested), 5-column eye lenses, per-character detail (Probe chest light, Solder hazard belt + rolled stack lips, Flux light lines, Pixel cheeks; soles, toe caps, thumbs, bevelled packs). 5 mesh parts (body, head, arms, legs, gear), 2 LODs: high drawn, low = the P16 set under the TR_LOD_NEAR bench bit (the runner never changes depth).

| character | high LOD tris (meshes + 16 eye + scarf side) | low LOD | drawn a frame, P16 -> P16b (tris / rows / px, host mean of 64 run frames) |
|---|---|---|---|
| PROBE  | 1334 | 874 | 426 / 5,911 / 14,971 -> 672 / 7,802 / 15,212 |
| SOLDER | 1454 | 858 | 419 / 6,161 / 16,925 -> 712 / 8,358 / 17,294 |
| FLUX   | 1200 | 792 | 380 / 5,088 / 12,791 -> 583 / 6,503 / 13,064 |
| PIXEL  | 1284 | 832 | 407 / 5,510 / 14,126 -> 632 / 7,168 / 14,316 |

Estimate at the silicon unit rates (setup 1.53 us/tri, ~134 cyc/row, 800 MHz; both split across the two cores): PROBE +0.35 ms/core, SOLDER (worst) +0.41 ms/core, skinning +~0.02 ms. Against the P16 silicon frame 20.5-22.2 ms: ~20.9-22.6 ms; with the P15 zones' ~4 ms that estimated ~24.9-26.6 ms against a drafted ~25 ms ceiling -- that ceiling was always a soft target sized off the P16 estimate, not the frame's hard limit; the hard budget at TR_PANEL_HZ=30 is 33.3 ms/frame. Measured on silicon (2026W36-0009, 30 Hz RAM-run, bca3345): frame 21.0-22.4 ms/core, 300 flips/10 s all on time, 0 slow, 0 underruns -- comfortably inside the 33.3 ms budget, and in line with the P16 baseline above (the estimate's ~1-2 ms overstated the P16b delta). Levers if a future character or scene needs the headroom back: fewer leg / arm segments, or TR_LOD_NEAR -- a single global quality bit, not P16-specific, that also drops nearby wall/deco/entity detail (walls(), tr_scene_deco(), the entity emit at r3d_scene.c:1817) and forces the runner to its low-LOD mesh; that low-LOD mesh is itself still welded with the P16b eye upgrade (16 eye tris, not the pre-P16b 8, lowest y -0.08), so enabling it is not simply a reversion to the old P16 cost.
Golden frame 2079 -> 2331 tris; synthetic worst case (16 wires packed near, crashing) 4107 tris: TR_DL_MAX_TRIS 4096 -> 4608 (DL 234 KiB, setup 630 KiB: render.c memory-map asserts hold). Max band bin 556 of 1024. Stack: tr_scene_build_part 24,560 B + tr_r3d_emit_mesh 8,144 B ~33 KB of the 64 KiB per-core stack (A32 -O2 -fstack-usage).
Renderer 332,888 B crc 0xEAB99923 (__bss_end 0x0256586C, 108,436 B under the 0x02580000 DL -- the earlier 0x02564A3C / 111 KB here was wrong, re-derived by cross-build against renderer.ld's own __bss_end symbol; this build also carries this round's roll_about_middle() yaw-term fix, +8 B of .bss for the test-only tr_front_pullback global).
HE /tmp/tr-p16-he30 222,992 B md5 7b16b7ce34217a93614acc4c1d5b4215 (BOARD alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he, TR_PANEL_HZ=30, AUTOLAUNCH, TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0); autolaunch id {0x02500000, 0x51458, 0xEAB99923} matches; no I2S/TAS2563/CC3501E code.

## Parts from the far end of the road (fix/obstacle-far, 2026-09-23)

Maintainer on glass (third time): "obstacles still too close. You should start drawing earlier". Measured in screen terms first (host, 720 x 1280, runner's lane, every zone): horizon row 443; the road melted into the glow 46-63 px under it (rows 489-506, ~5,200 deep, ~2.8 s out); parts spawned 7.0 s out and first showed 7-20 px under the horizon, in the glow ABOVE the visible road, 2-5 px tall; the pickup was recognisable (>= 16 x 6 px) only 2.0-2.2 s out.
Change: spawn row -440 -> -1540 (12.0 s out, ~21,700 deep; spawns still every 20 steps, same parts, at most 13 of 16 slots alive -- the idle replay dies on the same part with the same score, 100 steps later); the zone gate spawns at -1210 (fits an attract zone after the last gate's tail). World haze: the old curve up to view depth 3,600 (the textured ground and the shoulder fade unchanged: shimmer 5.22 %), then linear to 1 at 72,000; the board runs to the skyline's foot (23,800) as one stretched far tile (14 tris); walls melt into the board's colour 5,600 -> 8,800 and stop there (no extra wall cost). Parts grow and fade in from the road's colour over 0.25 s; far size up to 3.1x (obstacles), 2.6x (wires), 6.6x (pickup) at spawn.
After: the road stays visible (>= 40 off the glow, neon's dark board the lowest at 52) up to the skyline's foot, row 454 (11 px under the horizon), in every zone; a part appears 0.05-0.15 s after spawn standing at row 455, 12.0 s out; 8 s out it is 22 x 8 (memory canyon 18 x 6) low obstacle, 24 x 18 high, 14 x 19 pickup, 21 x 7 low wire, 21 x 22 high wire (test_r3d_ent_lead.c).
Cost at the silicon unit rates (TR_PROF_SETUP / TR_PROF_TRI / TR_PROF_GOURAUD), worst frames (SOLDER, 16 live parts, gate sweeping, crash): +0.11 to +0.21 ms/core; memory canyon crash 19.09 -> 19.21 ms/core est; max band bin 743 of 1024.
Renderer 408,512 B crc 0x04BC9454 -> 410,776 B crc 0xA1A8EA74 (__bss_end 0x02571BB0 -> 0x025724B0, 56,144 B under 0x02580000). HE /tmp/tr-far-he30 224,192 B md5 fb1082fe95db60e56ec19fa3c3f46c81 (BOARD alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he, TR_PANEL_HZ=30, AUTOLAUNCH, TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0); autolaunch id {0x02500000, 0x64498, 0xA1A8EA74} matches; no I2S/TAS2563/CC3501E code. Not yet on glass.

Round 2 (maintainer, on the host still: "This region is still not rendered" -- the far third of the road, the textured board ending ~2,900 deep and a flat haze-coloured strip, no scenery, beyond). Measured per far road row (skyline foot to ~3,100 deep, test_r3d_ent_lead.c): colour edges across the three lanes 0 in every one of the 58-67 rows in every zone (737f968), side scenery missing on a side in 6 (board) / 5 (antenna) / 21 (neon) rows. The parts were there all along: in a run's frame (a part a second, 12 s of approach) 12 of 12 grown-in parts show; the still had them bunched at 5-6 s.
Change: the far road is the zone's own texture to the skyline, through two more index slots (TR_MEM_A32_ZIDX 4 -> 6 x 16 KiB, the spare 0x022F8000..0x022FFFFF): each column averaged down the texture and over 3 columns (steady as the board scrolls: no sparkle), in three levels by depth (to 4,000 / 8,000 / the skyline), each widening the bright lines to ~1 px at its far end so the lane lines and rails stay drawn; ~30 tris for the whole far road. Side scenery goes on to the skyline as 2-triangle billboards past 9,000 (the mesh's width, height and mean colour, fogged), in every zone.
After: 6-13 colour edges in every far road row (antenna 6, neon 13), scenery on both sides of every row; parts 12 of 12. Cost at the silicon unit rates, worst frames, against the pre-unit baseline (this unit total): +0.66 to +1.14 ms/core; memory canyon crash 19.09 -> 20.16 ms/core est; max band bin 896 of 1024 (die city, zones test), DL worst 4,287 of 4,608.
Renderer 411,868 B crc 0x87B25167 (__bss_end 0x02573978). HE /tmp/tr-far-he30 224,192 B md5 51f34a2d6c0b8b80c7aedc37a139dd6d (BOARD alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he, 30 Hz, AUTOLAUNCH, TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0); autolaunch id {0x02500000, 0x648DC, 0x87B25167} matches; no I2S/TAS2563/CC3501E code. Not yet on glass.

Round 3 (maintainer on glass, memory canyon: "the bottom road grid is flickering"; plus an independent review of 871c472). Ground shimmer per zone, lanes / shoulders, near (250-700 deep) / mid (700-2,200) / far (2,200-6,000), running straight and changing lane, 30 Hz (tests/host/test_scene_shimmer.c, % of ground pixels a frame whose SSAA error jumps >= 32): before, die city lanes mid 18.1 / 19.1 %, far 17.3 / 17.7; memory canyon lanes mid 12.4 / 14.9, far 8.3 / 11.1, shoulders near 11.1 / 8.3, mid 8.3 / 5.8; the board's own (accepted) shoulders near 8.45, mid 5.2. After: every zone, band and motion <= 8.45 % (the board's shoulders near, unchanged), memory canyon <= 6.1 %, die city <= 6.3 %. Fixes: the memory canyon art has no 1-texel feature but the rails' low-contrast lit / dark edge lines (tools/genzone.py: 2-texel traces 2 apart, 2 x 3 via sleepers, 2-texel frames, 2 x 2 balls, 2-texel fly-by stripes, softer pads and silk; the bus-line look kept); the lanes' fog palette rows fade each colour toward its 5 x 5 neighbourhood's mix with depth (fade_detail(); zones.h nb from genzone.py), the shoulders toward the texture mean as before.
Review fixes: in real play (game + zone schedule driven, every zone, play and attract) band 14's bin filled -- 871c472 reached 1,011 of 1,024 in this drive, the reviewer's drive dropped 1,336 triangle-bands; now far parts past 9,000 are quads (arch posts + lintel, resistor body + legs, wire posts; wires in 2 segments), billboards under 3 px are skipped, the far walls are emitted last, and TR_BIN_MAX 1024 -> 1536 (bins 120 KiB, stacks moved to 0x025DE000..0x025FE000, the core-1 gate to 0x025FE000): worst 940 of 1,536 (test_scene_load.c), the reviewer's drive 984, 0 dropped. Far road past a gate averaged too (slots 6/7 share 4/5's rows; no pop at the crossing: a copy, not a recompute); the far rows and the neighbourhood tables built by genzone.py (a gate's spawn frame +0.2 ms host, was +5.6 ms). Billboards keep their part's area (a dish's quad narrower) and masts keep their 10-tri lattice: a tile's mesh -> billboard switch changes at most 108 px (antenna field), 6-52 px elsewhere. Gate row -1100 (201 steps + 12 tail of 224). unlit() on the plain FAR_FOG_END curve again.
Cost at the silicon unit rates, worst frames, this unit's total over the pre-unit baseline: +0.55..+1.09 ms/core (memory canyon crash 19.09 -> 20.02). Renderer 422,396 B crc 0x243DFE26 (__bss_end 0x02577300). HE /tmp/tr-far-he30 224,192 B md5 4f27af400f14bd4e77f72443bd1fe284 (BOARD alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he, 30 Hz, AUTOLAUNCH, TILT_TAKEOVER, shield e1m_evk_rk055hdmipi4ma0); autolaunch id {0x02500000, 0x671FC, 0x243DFE26} matches; no I2S/TAS2563/CC3501E code. Not yet on glass.
Round 3 re-review minors: the far parts' quads are a checked table (tr_far_quad_t; test_r3d_scene.c pins every edge to a vertex of that mesh's triangles of the quad's colour) -- which caught two wrong guesses: the arch's posts are 94..112 (not 87..112) and all colour 15, a wire post is a colour-6 stem (+-5) under a colour-7 head (+-11), not one +-11 quad. The shimmer test keeps a lower-bound control (board shoulders near > 4 %). Renderer 422,276 B crc 0x130AF57A (__bss_end 0x025772C0); HE /tmp/tr-far-he30 224,192 B md5 e7b028eccdebed57f7681a0846ea2422, autolaunch id {0x02500000, 0x67184, 0x130AF57A} matches. The PROBE colours in the 2026W36-0009 framebuffer captures were a torn capture (host render of 70d809d: the blue robot in every zone).

## ISA round on silicon (spike/a32-isa 76f91e6, 2026W36-0009, 30 Hz)

Changes in the build:
- `r3d_raster.c` at -O3 -funroll-loops;
- masked NEON span tails;
- register-resident row loop;
- VSTM band fills;
- exact `recip62`;
- constant-w ground span hoist;
- no-pixel-centre triangle reject.

Results, against feat/core:
- End-to-end frame **15.3-24.3 ms/core (mean 19.3)**, against 19.4-29.5.
- Prof unit rates:
  - noz_tex 23.9 -> 16.5 cyc/px;
  - bg 1.9 -> 1.2 ms;
  - setup 1261 -> 1002 cyc/tri;
  - Gouraud 26.2 -> 22.2 cyc/px.
- Golden build: pad3[6] [31:16] = 0, so the raster is **bit-exact on silicon**.
- payload-isa -O3u: 11/11 checks pass.
- The Gouraud lane-vector hoist gained nothing (GZR_9 27.3 vs GZ_CUR_9 27.1 cyc/px; GZH_4 42.7
  vs 19.7) and is reverted.

Details and per-commit estimates: `docs/superpowers/specs/2026-09-23-a32-isa-opportunities.md`.
