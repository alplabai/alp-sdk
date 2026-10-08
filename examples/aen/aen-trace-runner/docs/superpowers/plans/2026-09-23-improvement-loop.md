# Improvement loop — ledger

Every improvement runs the SAME loop, one git worktree + branch per unit. A
unit only advances when its stage gate is met; the orchestrator updates this
table at every transition (it is the recovery map after a context reset).

## The loop (per unit)

1. **Measure** — host numbers/renders of the current state (and silicon if
   the question is hardware). Written down before any change.
2. **Implement** — implementor agent in its own worktree; fail-first tests;
   real `tests/host/runner.sh` all PASS; renderer + 30 Hz HE build (BOARD
   aen803 verified in CMakeCache); images/GIF for a critical look.
3. **Review** — independent reviewer on `BASE..HEAD`. Findings -> fix round
   (same implementor) -> scoped re-review. Repeat until clean.
4. **Bench** — RAM-run on E1M-AEN803 2026W36-0009 (2026W36-0009): flip histogram,
   underruns, per-core frame ms, prof where cost changed. Numbers appended to
   docs/2026-09-22-measurements.md.
5. **Maintainer look** — shown on the panel; feedback -> back to step 2.
6. **Merge** — into feat/core (merge feat/core into the unit first if it
   moved; re-gate; golden re-derived, never hand-edited).
7. **Release** (batched) — Flow D release blob when a set of merged units is
   worth shipping; A32 parked in SRAM check, read-back diff, cold cycle,
   fresh read-back cmp, standalone boot check.

Bench is serial (one unit at a time on 2026W36-0009). Unit order into feat/core
follows readiness; later units merge feat/core before their bench.

## Units

| Unit | Branch / worktree | Stage | Notes |
|---|---|---|---|
| P16 characters (look, reactions, 4 skins) | merged | 6 MERGED into feat/core 26809dc (integration cda26d0; review clean; benched 19.4-29.5 ms; maintainer approved) | 6a83ce6 was reviewed clean; maintainer "still sharp edges, more detail" -> P16b rounded caps, more segments, weld, per-char detail |
| Obstacles visible early | merged | 6 MERGED into feat/core 26809dc (integration cda26d0; review clean; benched 19.4-29.5 ms; maintainer approved) | maintainer: "start rendering when close" / "start earlier" |
| P15 world zones (CPU die, memory canyon, RF field, neon night) | merged | 6 MERGED into feat/core 26809dc (integration cda26d0; review clean; benched 19.4-29.5 ms; maintainer approved) | |
| HW accel probe (GPU2D, DMA0, Helium ref) | spike/hw-accel / trace-runner-hwa | DONE (5357b85 A32 probe on silicon): NS A32 reads GPU2D HWREVISION 0x00000004 (HE 0x0FBE000B) -> HE owns GPU2D; A32 drives DMA0 via 0x490A0000: 40 bands 2 ch = 380 us A32 time vs NEON copy 1131.6 us; band load invalidate 4.6 + WB read 22.4 us/band (NC 483.5). Design: hwa docs/superpowers/specs/2026-09-23-gpu2d-ground-dma-design.md | decides GPU2D ground / DMA copy-out |
| Integration (feat/core + obstacles + P16b + P15) | merged | 6 MERGED into feat/core 26809dc (integration cda26d0; review clean; benched 19.4-29.5 ms; maintainer approved) | combined packet 172 B: 160 character,161 react,162 react_side,163 react_seq,164 react_ms u16,166 idle_ms u16,168 zone,169 pad,170 gate_y i16; flags CHAR b8, IDLE b9, ZONE b10; fit 512 KiB (4 bpp zone textures or far indices to spare 0x022E8000..0x022FFFFF); one combined build for the maintainer look |
| Obstacles appear far up the road | fix/obstacle-far / trace-runner-far (from feat/integrate) | 6 MERGED into feat/core (integrate2 4fdca47; reviews clean; benched 20.4-28.5 ms) | maintainer 3rd time: "obstacles still too close. You should start drawing earlier" — host contrast metric (6.8 s) did not match the eye; spawn farther + road visible farther + screen-space targets (appear within 60 px of horizon, recognisable >= 8 s) |
| Integration 2 (far road v3 + booth) | feat/integrate2 / trace-runner-int2 | 6 MERGED into feat/core (integrate2 4fdca47; reviews clean; benched 20.4-28.5 ms) |
| Edge anti-aliasing (maintainer: "still doesn't look smooth" -> "Jagged edges") | feat/edge-aa / trace-runner-aa (from spike/a32-isa) | PARKED default OFF at 566e424 (WIP; qemu +9.9..15.3 ms/core default, +5.8..9.7 with CREASE=0 WNEAR=1000; target +6 missed; full gate/builds not run). Was: 2 optimise (eedf771 4x MSAA on silicon: frame 17.8 -> 41.7 ms, MSAA bands 36 ms/core; quality edge |dL| 0.9-1.3 vs SSAA; default OFF; optimising: interior fast path, NEON sample DDA, 2x MSAA, depth-limited MSAA; target <= +6 ms/core) |dL| 0.9-1.4 vs today 2.7-6.4); silicon rows MSAA_INNER 14.7-17.9 cyc/px (= today GZ 14.6), MSAA_EDGE 33-36; z-edge MLAA 4-6 ms/core stays default OFF; re-estimating MSAA cost + plan) | maintainer 2026-09-24 |
| Art pass: soft/low-contrast look ("even old Nokia phones had a very good car race") | spike/art-soft / trace-runner-art (from feat/integrate2) | 6 MERGED into feat/core (spike/art-soft df0b8d0; review clean; bench df0b8d0 18.8-23.0 ms, 5368/5369 one-refresh flips, 0 relaunches, all 5 zones, no seams). RULING 2026-09-24 toy_crisp_mid default (maintainer: "You choose. I think enough for a demo"); benched on 2026W36-0009 30 fps, flips 1532/1533 one refresh, A32 17.8-23.9 ms; art iteration STOPS; merging | maintainer 2026-09-24 |
| GPU2D road layer | feat/gpu2d-road / trace-runner-gpu | PARKED unmerged at e8afbb6 (fix round 2 done: gpu_stop leaves clock gated, recover only if idle, chunked CRC, tests O/P/R; 90 PASS; UNREVIEWED + unbenched: gating-stops-writes unproven) (maintainer: graphics enough for demo; GPU road dev-only: booth art only via J-Link). Re-review 919d747 found gpu_reset not an abort. Was: 3+4 (7722f87 bench 5: plan+patch 3.00 -> 0.95 ms (split + WB alias); A32 frame 14.2-14.5 ms vs 18.9-19.2 A32-ground = ~4.7 ms/core saved; self-checks 0; clean picture; review running; art seam pending art pick) | ~7.3 ms/core of A32 ground raster |
| ~~M55-HP as 3rd raster core~~ | — | DROPPED | maintainer 2026-09-23: M55-HP is reserved for game sound + camera + Ethos-U NPU (body control); never a raster core |
| A32 NEON math (transform, recip) | — | 0 (after P15: shares r3d_raster/r3d_math) | |
| A32 ISA / NEON opportunities (maintainer: "ARM instructions and so on") | spike/a32-isa / trace-runner-isa | 6 MERGED into feat/core (integrate3 42c1f1d; combined build benched 17.7-25.2 ms mean 20.6 vs 20.4-28.5 before; golden bit-exact on silicon) | weighted to gouraud/setup/per-row/sky since ground may go to GPU2D |
| Algorithm-level (overdraw/Hi-Z/span buffer, cone culling, dynamic band split, sky/horizon caching, per-tile part cache, hand asm) | spike/algo / trace-runner-algo | 1 DONE (f958546, host model 0 px mismatch over 910 frames, 15.2 vs ~15.5 ms silicon): hidden ground 21 % of ground shading; z-tested 44 % wasted; runner ~50 % backfaces; bands ALREADY dynamic (atomic); scene build = emit 91 %. Ranked: ground per-span persp/fog 0.8-1.1, ground-last where z==0 1.0-1.6, Gouraud NEON regs 1.0-1.5, coarse-z tiles 0.35 avg/1.46 worst, row walk asm 0.6-0.9. Rejected: sort, deferred, span buffer, part cache, normal cone. -> (a)(b)(c) queued as ISA phase 3 | maintainer: "assembly? another type of algorithm" |
| Sharpness ("lower resolution graphics than the screen") | spike/sharp / trace-runner-sharp | 1 DONE (beead8a: cause = near ground 128x128 nearest, texel up to 3.6x7.3 px below row 740; edge aliasing only 1.3-3.7 %; sky already dithered). Maintainer chose GPU2D road with 256x256 bilinear art | maintainer 2026-09-23 |
| DMA band copy-out | feat/dma-copyout 19758d1 (kept, NOT merged) | PARKED after silicon A/B on 2026W36-0009: DMA0 works from the renderer (status 0xC0000200, 0 faults, 40/40 bands by DMA); core-0 copy-out per frame: NEON 45.5-46.4k ticks (0.46 ms), DMA double-buffer 35.9-37.6k (0.37 ms), DMA single 96-109k (1.0 ms). Saving 0.09 ms/core does not pay for ~90 KB CBAND1 + DMA code; revisit only if copy-out grows | est. 0.3-0.5 ms/core |
| Booth features (high scores + tilt name entry, difficulty ramp, pickup combos, crash shake) | feat/booth / trace-runner-booth | 6 MERGED into feat/core (integrate2 4fdca47; reviews clean; benched 20.4-28.5 ms) | maintainer pick 2026-09-23; scores RAM-only this unit, persistent storage = separate decision |
| Polish: runner AA outline + blob shadows | — | 0 (after obstacle-far v2 + ISA: shares r3d_scene/r3d_raster) | maintainer pick |
| SoM showcase HUD (per-core load A32x2/HE/GPU2D, board power mW, frame time) | — | 0 (after booth: shares HUD) | maintainer pick |
| Boot watchdog robustness | fix/boot-watchdog / trace-runner-wd | 6 MERGED (ae9910a) + FLASHED as release v5.2 (a32_app md5 e7b4660b1990800094d58697dc20d3e5, atoc 0x80540000 md5 9ffe28aaeb4183d9a5b32e77652bfd88; cold read-back OK; 3 cold boots: 0 commands, renderer adopted, 30 fps). NEW FINDING: A32 stub only alive ~8.8-9.0 s after HE boot (HE uptime ~9.1-9.2 s) -> ~9 s dark at power-on; A32 launch->first frame 95 ms | release v5 boot: 8 watchdog commands before the renderer settles; budget could end HALTed |
| Power-on: A32 stub alive only ~9 s after HE boot | — | 0 (measure where TF-A/SE/A32 boot time goes; booth power-on shows nothing for ~9 s) | found 2026-09-24 |
| OSPI NOR (maintainer 2026-09-24: "one ospi nor flash available") | — | 0 (candidate store for high-score persistence and GPU art blob; ISSI IS25WX256-JHLE 32 MB on OSPI0 SS1, works on 2026W36-0009; XIP window 0xA0000000 shared with non-working HyperRAM SS0) | decision with score persistence |
| NPU body control (maintainer: "We will use NPU to detect person movement"; camera later) | feat/npu-body / trace-runner-npu | silicon probe BLOCKED: readback OK (s38/mram-before.bin md5 98a1db2bab7a366618bb92e273825b89 = release v5); write aborted by alp-sdk ATOC guard: 2026W36-0009 SE-UART (FTDI <adapter-serial>) not enumerated and bench-env exports SE_UART="None" -> needs adapter replugged or bench-env fix. Nothing written. | camera moves to the display board later (maintainer) |
| Release v5 (30 Hz + merged units) | rel/* | 7 FLASHED 2026-09-24 (feat/core f576798: a32_app 0x80020000 md5 ffa4386b1c762bef25448b6ba99639c7 458,752 B, atoc 0x80544000 md5 437b36d862c1e6c08ed570d2c7c5b19d; bl32 unchanged; read-back after cold cycle identical; pre-v5 read-back local work directory s36/mram-live.bin md5 5652e9c8f419a2534912b92300e64e93). Boots standalone BUT HE watchdog HALT/LAUNCH x4 at boot (stub not parked + 100 ms first-frame timeout) -> fix/boot-watchdog | Flow D; renderer grew — recheck a32_app layout |
| Camera OV9281 on 2026W36-0009 | — | DONE 2026-09-24: moved to 2026W36-0009; FFC CLK pin was touching GND, cable replaced; 640x400 GREY8 60/60 frames ~101 fps; needs I2C1 re-mux cure (P3_7/P7_2 fn0->fn5) + software AEC | unblocks NPU body control |
| Restore person_detect slot0 | — | project end | md5 5839e003d5d069f6fd912eb22774d037 |
