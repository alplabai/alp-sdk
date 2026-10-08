# NPU body control — design (phase 1: host + build, no camera)

**Status:** host prototype and silicon probe built; nothing here has run on silicon.
The camera is on `the E1M-AEN803 2026W36-0001 EVK`, the display on `the E1M-AEN803 2026W36-0009 EVK`, so phase 1 is proven
without a camera. Every latency figure marked *Vela* is Vela's estimate, not a measurement.
`probe/npu` exists to replace those estimates with DWT numbers from the HP.

**Goal:** a person in front of the booth steps left or right to change lane, jumps to jump
and crouches to duck. The target is **≥ 15 Hz** and **< 60 ms camera-to-intent**. The
game's input path does not change: the NPU produces the same `tr_box_t` that
`src/vision/detect.c` produces today, and `track.c` / `mode.c` / `attract.c` consume it
unchanged.

Core allocation (binding): the **M55-HP** owns the camera, the Ethos-U NPU and game sound. The
**M55-HE** owns the display, the HUD and the game. The **A32 pair** runs the 3D renderer.

## 1. Model choice, by measurement

Toolchain: Vela 5.2.0 (`ethos-u-vela`, local venv `.venv/`) with Alif's
`ensemble_vela.ini`, which is not redistributed. The NPUs are:
- **HP:** `ethos-u55-256`, `RTSS_HP_SRAM_MRAM`, 400 MHz.
- **HE:** `ethos-u55-128`, `RTSS_HE_SRAM_MRAM`, 160 MHz.
- **U85:** `ethos-u85-256`, `Ethos_U85_SRAM_MRAM`, 400 MHz.

All runs use `--memory-mode Shared_Sram`: weights in MRAM, arena in SRAM. The E8 has the
HP-paired U55 at 256 MAC according to `metadata/socs/alif/ensemble/e8.json`, but only the HE's
U55-128 has been read on silicon.

The latency columns are the Vela NPU estimate only. The CPU column lists what Vela leaves on
the M55.

| model | input | licence | NPU / CPU ops | HP U55-256 | HE U55-128 | U85-256 | MRAM (Vela'd) | SRAM arena |
|---|---|---|---|---|---|---|---|---|
| **MoveNet SinglePose Lightning int8 v4, cut to its NPU body (chosen)** | 192x192x3 int8 | Apache-2.0 | 119 / **0** (100 %) | **9.22 ms** (`--optimise Size`; 9.12 Performance) | 36.12 ms | 9.55 ms | 2,429,520 B | **277.5 KiB** (Size; 1084 KiB Performance) |
| MoveNet Lightning int8 v4, as published | 192x192x3 uint8 | Apache-2.0 | 150 / **29** (16 %): float CAST, QUANTIZE, ARG_MAX, GATHER_ND, SQRT, DIV, ... | 10.10 ms + CPU tail | 41.05 ms + tail | 9.93 ms + tail | 2,420,064 B | 1084 KiB (569 Size) |
| yolo-fastest_192_face_v4 (emza-vs ModelZoo) | 192x192x1 int8 | Apache-2.0 | 108 / 0 (100 %) | 2.78 ms | 11.89 ms | 2.53 ms | 369,792 B | 361 KiB (102 Size) |
| SSD MobileNet v1 COCO quant (TF, 2018, uint8) | 300x300x3 uint8 | Apache-2.0 | 60 / 1 (`TFLite_Detection_PostProcess`) | 13.37 ms | 55.97 ms | 13.55 ms | 3,131,776 B | 1760 KiB |
| SSD MobileNet v1 int8 (Arm ML-zoo) | 300x300x3 int8 | Apache-2.0 | 60 / 3 | 21.07 ms | 86.35 ms | 21.08 ms | 6,118,272 B | 2113 KiB |

The published MoveNet's 29-op float tail cannot run through alp-sdk's `<alp/inference.h>` as
it stands. `src/backends/inference/tflm.cpp` registers a fixed 32-op resolver with no CAST,
GATHER_ND, ARG_MAX, FLOOR_DIV, SQRT, DIV or PACK. It is also float work over 48x48x17 maps,
estimated at roughly 10–25 ms on the M55. That estimate is not measured.

`tools/movenet_cut.py` removes the tail:
- The graph is kept from the int8 input (`tfl.quantize`, q = grey − 128) to the four int8 maps
  the tail reads: centre, heatmaps, offsets and regression.
- The result is **100 % NPU**, so it runs through the same single-Ethos-U-op path as the
  silicon-proven `person_detect`, with no SDK change.
- The tail is reimplemented as integer C in `src/vision/movenet.c`. It is 872 B of M55 text.
  It computes a square root on only 126–614 of the 39,168 cells per frame, because the search
  starts at the regressed cell and skips any cell whose heat could not win.

Why MoveNet:
- **It gives the skeleton.** Lateral position comes from the torso span. Jump is the
  head/shoulder line rising while the ankles rise too. Crouch is that line dropping while the
  ankles stay down.
- **Raising the arms is never read as a jump.** The top edge comes from head and shoulder
  keypoints, never from a wrist.
- **It works with the legs out of frame.** At about 2 m the OV9281 does not see the whole body,
  and the bottom edge then falls back to the frame edge.
- **The skeleton is also the SoM showcase** (section 6).

Why not the others:
- **yolo-fastest face:** 3.3x faster and 6.6x smaller, and a natural match for the mono sensor.
  But it sees faces only:
  - The face drops out when a player turns their head.
  - At 3 m a face is about 12 px of the 192 input, which is the bottom of its anchor range.
  - It cannot tell a crouch from a lean.

  Keep it as the fallback if MRAM or DTCM runs short. It also drops into the same probe.
- **SSD person:**
  - Two to three times the latency and six to eight times the arena (1760–2113 KiB), which
    has no home in the A32 build's SRAM0 map (section 2).
  - It needs a CPU post-process op.
  - On the clip its standing box height jitters 280–320 px (13 %), against a duck threshold
    of 80 %.
  - It is a box only. The classical `detect.c` already gives a box for free.

**Licences.** MoveNet (Google, Kaggle `google/movenet/tfLite/singlepose-lightning-tflite-int8/1`,
sha256 `cd7cc22fa946e5d146a7b98d496853e1923e22828d3972d579973f27f91bb105`) is Apache-2.0,
and so is the cut derivative (sha256 `6099cdcdff295e25a59107a5318df92f79cfc58ea8c366f5a806f53cf753e898`).
Apache-2.0 is fine for a commercial demo with attribution and a NOTICE entry. yolo-fastest
(emza-vs/ModelZoo) and both SSDs are also Apache-2.0. **No weights are committed:** the tools
fetch and cut them.

## 2. Pipeline on the M55-HP

```
OV9281 640x400 GREY8, ~100 fps  ->  CPI  ->  2 x 256,000 B in SRAM1 0x02480000 (the reserved camera pool)
  -> HP: drop stale frames, keep the newest                       (camera.c FIFO: 1 frame of lag otherwise)
  -> tr_movenet_input(): letterbox into 192x192x3 int8 in the arena  [2x2 mean, q = grey - 128; Helium later]
  -> Ethos-U55-256: cut MoveNet, weights read from MRAM            [9.22 ms Vela]
  -> tr_movenet_decode(): 17 keypoints in frame px                 [integer, <= ~1 ms estimated]
  -> publish to the pose slot (section 4)                          [~200 B]
```

- **Rate:** as fast as frames arrive and the pipeline allows, about 12 ms per pose, so roughly
  50 Hz with the sensor at 100 fps. The HE consumes on its 33 ms tick (`TICK_MS`), so the pose
  it reads is at most one pipeline pass old.
- **Exposure:** the OV9281 has no usable on-chip AEC. It was bench-probed and exposure and
  gain never move on their own. The HP runs software AE instead: a P-controller
  (`src/vision/camera_ae.c`) on the RAW captured frame's mean (not the letterboxed one, which
  is biased by its own padding -- that file's own header comment), written every
  `TR_AE_PERIOD` frames (about every 0.5 s at the pipeline's sustained rate). Exposure is in
  WHOLE LINES (`OV9281_FETCH_EXP_H/M/L`'s own 4/8/4-bit split across 0x3500/01/02 already does
  the sensor's 1/16-line scaling internally -- a value here is never pre-shifted), written
  DIRECTLY over I2C to bypass `ov9281_set_ctrl()`'s own clamp (a real gap: that clamp uses
  `mode->vts - OV9281_EXP_MAX_OFFSET`, which the driver never re-derives per mode change at
  runtime the way this app's own live VTS read does), with a group hold
  (0x3208 start/end/launch) around the exposure + gain writes so the sensor applies both
  atomically. The ceiling itself is derived live from VTS (read over I2C, since it is genuinely
  per-mode: 100/50/100 fps, three different VTS values in `chips/ov9281`'s own mode table), not
  a fixed constant -- `tr_ae_exposure_max_from_vts()`, host-tested. Every touched register is
  read back and published in the beacon (`hp_dbg_t.ae_reg_*`) for bench confirmation.
- **Resolution:** 640x400 is kept. The letterbox makes it 192x120. A 1280x800 mode buys
  nothing, because the model input is only 192 wide.

**Latency budget**, camera to intent available on the HE:

| stage | ms |
|---|---|
| sensor readout (100 fps mode) | ≤ 10 |
| queue (newest-frame policy) | 0–10 |
| pre-process | 1–3 (estimated from HP SRAM read bandwidth, 43 MB/s copy) |
| NPU | 9.2 (Vela) |
| decode | ≤ 1 (estimated) |
| slot publish, then HE read on its next tick | 0–33 |
| **total** | **~21–66, typically ~40** |

The 60 ms target holds except in the unlucky tick phase. If that tail matters, the HE can
read the slot at the start of its tick rather than the end. An MHU doorbell buys nothing
here, because the HE acts only on its tick.

**HP memory:**
- **ITCM (256 KiB):** code. The probe image is 174,728 B with TFLM and the Ethos-U driver.
- **DTCM (1 MiB):** stacks, and sound's 91,820 B.
- **SRAM0:** the arena, 277.5 KiB. The SDK requires an NPU arena in SRAM0, per the
  `alp_inference_config_t.arena` doc.
- **SRAM1:** frames, in the reserved `0x02480000..0x024FFFFF` pool.
- **MRAM:** model weights, read in place. Pass B of the probe proves this or disproves it.

**SRAM0 caveat.** SRAM0 has no free 277.5 KiB hole in the A32 build's map. The map is:
- FB A: `0x02000000..0x021C1FFF`.
- Free: `0x021C2000..0x021FFFFF`, which is 248 KiB.
- The renderer's scratch and DL.
- 12 KiB and 32 KiB spares.

The candidates for the arena:
- (a) The `0x0233A804..0x0237EFFF` spare, about 274 KiB. Short by 3.5 KiB, so it needs the
  DL trimmed.
- (b) Prove that the NPU can reach HP DTCM through `local_to_global()`. The backend remaps
  addresses, but the SDK doc says SRAM0 only.
- (c) SRAM1, above FB B's end at `0x027C2000`, which leaves too little.

The probe runs its arena in SRAM0 because it runs standalone. Deciding this is the first
integration task after the probe passes.

**Risks:**
- **I2C contention.** Camera SCCB on the HP could share the carrier I2C with the HE's BMI323,
  the same hazard `docs/2026-09-23-sound.md` records for the amp init. Check the J5 SCCB bus
  first.
- **CSI on the HP is unproven.** The camera path is proven only on the HE (PR #2247). The HP
  needs the CPI/CSI DT nodes and IRQs on `rtss_hp`.

## 3. Motion logic (HE, pure C, host-tested)

> **Superseded by #2788** (the arm-raise controls): lane and jump are no longer read from the
> torso, so the lane-band, jump and `TR_CAM_MIRROR_X` items below no longer exist; lane and jump
> are arm raises (`src/vision/arms.h`) and only the duck is read from the torso. The rest of this
> section is the design history it was built from.

`src/vision/pose.c` turns a pose into a torso box; `track.c` reads intent from it. Revised
2026-09-25 after the 2026W36-0009 finding "it always sees me jumping": with the legs out of frame the
first box (head top to ankle, else frame bottom) read a player walking toward the camera as a
4 s jump, against a stance baseline captured once at boot. The contract and constants live in
`src/vision/track.h`; `tests/host/test_track_intent.c` replays the silicon keypoints.

- `tr_pose_box()`:
  - **Torso box:** x + w/2 = torso centre x, w = shoulder width, y = shoulder-mid y,
    h = hip-mid y - shoulder-mid y (0 when no hip is confident). Head, arms and legs are never
    read.
  - **Confidence:** the mean score of the keypoints used.
  - **Validity:** at least one confident shoulder.
  - The keypoint threshold is `TR_POSE_KP_MIN` = 77 (0.30).
- `track.c`, scale-invariant (s = torso length, or baseline scale x shoulder-width ratio):
  - Three lanes by thirds, each with a ±`TR_TRACK_HYST_PX` dead band, from torso centre x.
  - Jump: torso centre up > 25 % of s against the rolling baseline AND within ~300 ms, with s
    within 10 % of the baseline (walking closer grows s: no jump). Held until back down, at
    most 0.8 s.
  - Duck: torso centre down > 30 % of s, s not grown past 10 %.
  - Two consecutive frames to start, at least ~170 ms on, 6-frame landing cooldown.
  - Rolling baseline (EMA, ~1 s) while neutral; re-seeded on re-acquire or a > 30 % scale change.
  - `TR_CAM_MIRROR_X` and `TR_CAM_FLIP_Y` for mounting.
- **Per-player calibration.** `tr_still_step()` arms `tr_track_calibrate()` once the box has
  held within 15 % of its height (the torso length) for `TR_STILL_FRAMES` = 30 frames (1 s).
  - This replaces main.c's current "calibrate on the newest box after 2 s" rule, which was
    shaped by background subtraction's need for the player to move.
  - A pose model needs the opposite: stand still.
  - The banner text changes accordingly, from "move" to "stand still".
- **Attract takeover and walk-away.** No new logic. A valid pose makes `detected_now` true, so
  the reversible attract sub-state (`game/attract.h`, `TR_ATTRACT_ENTER_TICKS`) hands over as
  it does today. An invalid pose for `TR_TRACK_LOST_LIMIT` pauses the game, and continued
  absence returns to attract.
  - One change: re-arm the stillness calibration on every takeover, so each new player gets
    their own stance.
  - A stale pose counts as invalid: slot `seq` unchanged for more than 5 HE ticks.

## 4. HP -> HE channel: a latest-value pose slot

Only the newest pose matters, so this is a seqlock slot, not a queue:
- **Location:** `TR_MEM_PSLOT` = SRAM0 `0x0237F200`, reserved in `src/ipc/tr_memmap.h`. It sits
  in the sound ring's 4 KiB page, which the A32 never maps.
- **Access:** both M55s run `CONFIG_DCACHE=n`, so no cache maintenance is needed.
- **Writes:** the HP writes `seq` odd, then the body, then a barrier, then `seq` even.
- **Reads:** the HE copies the slot and accepts the copy if `seq` was even and unchanged
  across it.

| offset | field |
|---|---|
| +0x00 | `magic` 'TRPS', `version`, `seq`, `frame_no` |
| +0x10 | `tr_pose_t` (17 x {x, y, score}, 102 B), then `infer_us`, `pre_us`, `hp_state` |
| +0x80 | 64x40 GREY8 thumbnail of the letterboxed frame (2,560 B, to `0x0237FC7F`), for the HUD |

No MHU doorbell (section 2). The existing mailbox and doorbell work
(`project_aen_mhuv2_mailbox_driver`) stays available if the HE ever needs to wake on a pose.

## 5. What changes in the game (next phase, not in this commit)

- `main.c` gains `TR_INPUT_NPU` sourcing: the HE's `capture_box()` reads the slot and calls
  `tr_pose_box()`.
- `detect.c` stays as the no-HP fallback. `tr_ctl_select_mode()` takes "pose slot live" in
  place of "camera open".
- The HP firmware becomes camera, then NPU, then slot, beside the existing sound ring
  consumer. The HP_APP of the release ATOC is today `m55_stub_hp`
  (`a32/release/e1m-aen-evk-trace-runner.json`).

## 6. HUD: what the NPU sees (SoM showcase)

Proposal: a 128x80 tile in the HUD L2 corner (`TR_HUD_FB`, ARGB4444, the existing dirty-tile
repaint).
- **Content:** the slot's 64x40 thumbnail at 2x, as a dim silhouette, with the 12-bone stick
  figure drawn over it in the accent colour. Bones are drawn only between keypoints ≥
  `TR_POSE_KP_MIN`.
- **Caption:** a "NPU 9.2 ms · 40 Hz" line under the tile, from the slot's `infer_us`.
- **Behaviour:** it repaints only when `seq` changes. The skeleton is also the calibration cue:
  it turns green once `tr_still_step()` is counting.
- **Why it works as a demo:** it shows the edge-AI path live, on the chip, next to the game.

## 7. Host prototype — results

`tools/npu_body_proto.py` builds a 304-frame, 30 Hz synthetic 640x400 GREY8 clip:
- **The people:** two public-domain photographs, a standing man (Library of Congress
  LCCN2005681267) and a crouching man (SDASM 23_0073712, no known restrictions), both via
  Wikimedia Commons.
- **The script:** empty, walk in, stand 1.3 s, left, right x2, centre, jump (70 px peak, 0.47 s),
  crouch 1 s, walk off, empty.
- **Scene:** composited on a synthetic booth background, with σ = 3 sensor noise.

It runs the firmware path on every frame:
1. `tr_movenet_input()` (the C pre-process, via ctypes).
2. The cut model on LiteRT.
3. `tr_movenet_decode()`.

It checks the result against the full published model on the same pixels.

- **Intents over the clip** (`tests/host/test_pose_clip.c`, which replays the trace in
  `tests/host/tr_pose_clip.h`):
  - Calibrated at frame 48, after 30 still frames.
  - Step left gives exactly one −1 (frame 64).
  - Step right gives +1, +1 (frames 94, 101).
  - Back to centre gives −1 (frame 134).
  - Jump fires on frames 162–171; take-off was at 160.
  - Duck fires on 30 of 30 crouch frames.
  - No false jump, duck or lane change in any standing span.
  - The walk-off gives −1 (frame 254). Nothing is acted on after it, and the player is lost by the end.
- **Decoder accuracy** (`tests/host/test_movenet.c`, two frames of raw maps in
  `tests/host/data/`):
  - Against MoveNet's tail in float on the same maps: every keypoint within 2 px (|dx|+|dy|)
    and score ±1.
  - Against the full published model on the same pixels: every confident keypoint within
    3 cells, and the torso within 2 cells. The published tail quantises its distance map in
    0.24-cell steps, so it breaks near-ties a cell away.
  - Across the clip the max error on confident keypoints is typically 9–23 px, worst 37 px.
- **Presence separation** (min–max over the clip):

  | model | present | absent |
  |---|---|---|
  | MoveNet torso score | 63–158/255 | 5–36 |
  | face model | 0.16–0.75 | 0.02–0.05 |
  | SSD person | 0.26–0.76 | 0.08–0.11 |

- **Synthetic poses** (`tests/host/test_pose.c`): box geometry, raised arms, legs out of frame,
  no person, one-sided torso, stillness (jitter, move, loss), and lane, jump, duck and noise
  through `track.c`.

**Limits:**
- The clip is two still photographs moved around the frame, not video of a real jump. A real
  jump also bends the knees.
- The camera geometry is assumed: a 70° HFOV and 3 m to the player. It has not been measured.

## 8. Silicon probe (no camera)

The probe is `probe/npu/`; see its README for the full recipe. It is an M55-HP image,
ITCM-loaded through the SE (HP_APP, `loadAddress 0x50000000`). It reads a 3,197,968 B MRAM
payload at `0x80100000` containing:
- The Vela'd cut model.
- Empty, standing and crouching 640x400 frames.
- The host's expected map CRCs and decoded pose.

It runs two passes: A with the model copied to SRAM0 (the proven placement) and B with the
weights read in place from MRAM. For each frame it records pre-process, invoke (min/avg/max
over 16) and decode times from the DWT, the CRC match of each output map, the torso error
against the host, and presence. It also records the sustained Hz. Results go to
`TR_MEM_PSLOT` and to the RAM console.

**PASS criteria:**
- Both passes open and invoke.
- Every frame's presence matches the host.
- Torso error is ≤ 27 px.

Map CRCs are informational, because an NPU is not bit-exact to the TFLite reference kernels.

**Flow:** `probe/npu/flash-probe.sh`, built on alp-sdk's Flow D sector-pad, proof and race
machinery:
1. `readback` saves the full MRAM image.
2. `write` writes the payload at `0x80100000` and the probe ATOC (`0x80553FC0..0x8057FFFF`,
   built in a private SETOOLS copy), resets, and reads the results.
3. `restore` puts back every touched sector, `0x80100000..0x8040FFFF` and
   `0x80550000..0x8057FFFF`, from step 1's file.

The whole plan has been dry-run on the host with `FLOWD_DRY_RUN=1`. No probe was touched and
no MRAM was written.

## 9. Open items

1. Run the probe on 2026W36-0009. It reports the U55-256 invoke time, MRAM-direct weights, the
   pre-process and decode cost, and the sustained Hz.
2. ~~Place the arena in the A32 build's map (section 2).~~ **Done** -- see section 10.
3. HP camera bring-up: the CSI/CPI DT on `rtss_hp` -- **blocked**, see section 10; software AE
   is written (`src/vision/camera_ae.c`, `hp_vision/src/main.c`), untested on silicon.
4. A real-video check once the camera is back, recorded on the booth rig.
5. The HUD tile (section 6) -- not built; `hp_vision` publishes the 64x40 thumbnail into the
   pose slot (section 4's `+0x84`, corrected from the table's `+0x80` -- see `tr_pslot.h`), the
   HE-side HUD draw is still open.
6. Add a MoveNet NOTICE entry to the release.
7. `main.c`'s calibration still uses the original "hold a box, 2 s" rule, not section 3's
   `tr_still_step()`/`TR_STILL_FRAMES` stillness gate -- `pose.c` has both, `main.c` wires
   neither in yet (narrower ask than section 3 describes: swap the `tr_box_t` *source*
   only, task scope for this round).

## 10. Phase 2: packaging, HP app, arena -- implementor report

**Arena placement (item 2, resolved).** Candidate (a) was short by 3.5 KiB (273.996 KiB spare
vs 277.5 KiB needed) even after the `feat/core` merge changed the SRAM0 map (`TR_MEM_A32_DL`
moved into SRAM0, `ZIDX` grew) -- DL1's own footprint (`sizeof(tr_dl_t)`, confirmed by compiling
`r3d.h`: exactly 239,620 B = `0x3A804`, matching the spare's start to the byte) was unchanged by
that merge. Closed by trimming `TR_DL_MAX_TRIS` 4608 -> 4528 (-80, -1.7%; `r3d.h`), which shrinks
`sizeof(tr_dl_t)` by 4,160 B -- comfortably past the 3,588 B shortfall. `render.c`'s existing
`tr_dl_dropped` counter (golden-tested at 0) already turns "DL1 overflows its cap" into a counted
drop, not corruption, so this is a bounded, monitored cut, not a silent one; it needs a bench
check against the packed-crash worst case the 4608 figure was originally raised for (P16b).
`TR_MEM_NPU_ARENA` = SRAM0 `0x023397D0`, size `TR_MEM_NPU_ARENA_SIZE` = 284,160 B (277.5 KiB
exactly), 16-B aligned, 560 B of margin before `TR_MEM_ARING`/`TR_MEM_PSLOT`. `render.c` asserts
both directions at compile time (`DL1` <= arena <= `ARING`) -- a future change to either the DL
budget or the arena size that collides fails the A32 renderer's build, not silently.

**MRAM layout.** `bl32` `0x80002000`, `a32_app` `0x80020000` (~458 KiB, ends well under
`0x80100000`), **MoveNet weights `TR_MOVENET_MRAM_ADDR` = `0x80100000`, size
`TR_MOVENET_MRAM_SIZE` = 2,429,520 B** (`src/vision/movenet_mram.h` -- same address `probe/npu`'s
own payload already uses; the probe and the real `hp_vision` image are never resident at once, so
sharing the address costs nothing), then the ATOC package (`HP_APP`/`HE_APP` inside it, ending at
`0x80580000`, the System MRAM base). The model is read in place by the NPU (pass B, no SRAM copy)
and is **not** part of the ATOC -- `a32/release/build-release.sh` (`TR_HP_VISION=ON`) writes it as
its own Flow D item, sector-checked against `bl32`/`a32_app`/`atoc` for overlap the same way those
three already are. person_detect's slot0 region is untouched by any of this (different ATOC
entirely) -- restore its md5 `5839e003d5d069f6fd912eb22774d037` at the end of the project, per the
maintainer's standing instruction; nothing in this phase moved it.

**HP camera DT blocker (item 3).** `hp_vision` fails at the devicetree stage, not C compilation:
`e1m_evk_rpi_csi`'s `boards/` directory (`alp-sdk-lcd/zephyr/boards/shields/e1m_evk_rpi_csi/boards/`)
ships only `alp_e1m_aen80{1,3}_m55_he_..._rtss_he.overlay` (one line each,
`#include "e1m_aen.dtsi"`). `e1m_aen.dtsi` itself names only SoC-level nodes (`&csi`, `&i2c1`,
`&cam`, `&dphy`, `&gpio12`, `&pinctrl`) -- core-agnostic on their face. The fix this phase could
not make (out of this repo's scope; `alp-sdk-lcd` is a sibling repo) is almost certainly one new
file, `alp-sdk-lcd/zephyr/boards/shields/e1m_evk_rpi_csi/boards/alp_e1m_aen803_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay`,
containing exactly `#include "e1m_aen.dtsi"` (mirroring the HE variant byte for byte) -- but
**this is inferred, not proven**: it removes the DT parse error this phase reproduced
(`undefined node label 'csi_interface'`), not a claim that the CPI/CSI IP is actually reachable
from the HP core in silicon (design sec 2's own "CSI on the HP is unproven" caveat still stands
once the DT parses).

## 11. First silicon results (2026W36-0009, commit 563e178, 3 cold boots) -- fix round 5

**NPU works.** `hp_state` RUNNING, AE converged (thumbnail mean ~125), game 30 fps,
`tr_dl_dropped` 0. Real numbers vs this doc's Vela/estimate figures (section 1/2's table):

| stage | design estimate | measured | fix round 5 |
|---|---|---|---|
| HP loop rate | -- | 26 Hz | (a consequence of the two rows below) |
| `infer_us` | 9.22 ms (Vela) | ~16.4 ms | not touched -- real NPU invoke time, not a host-side cost |
| `pre_us` (letterbox) | 1-3 ms (estimated) | ~10.0 ms | **fixed**: `tr_movenet_input()`'s column source index
  (`sx`/`x1`) depends only on `frame_w`, the SAME for every one of `TR_MN_IN` (192) rows -- the
  original recomputed both divisions per PIXEL (36,864 times) instead of once per column (192
  times), a 192x more divisions than needed. Precomputed into a table once per call. |
| `decode_us` | <= 1 ms (estimated) | 7.4-11.2 ms | **fixed**: the per-cell "could this even win"
  filter (`(h << 16) / 29u < best`) ran a DIVISION on all `TR_MN_CELLS * TR_POSE_KP` (2304 x 17 =
  39,168) cell/keypoint pairs every frame, unconditionally -- the exact division the design's own
  section 1 comment says the search-from-the-guess order exists to make rare. Algebraically
  identical (`a/29 < b` <=> `a < 29*b` for non-negative integers) rewrite to a multiply, recomputed
  only when `best` changes (rare) instead of every cell. Host-verified byte-identical output
  (`tests/host/test_movenet.c`, `test_movenet_input.c`, `test_pose_clip.c` all still pass). |
| `thumb_us` | -- | ~0.27 ms | not touched -- already cheap |

Neither fix has a silicon before/after measurement yet (no hardware from this implementor) -- the
host-side division counts removed (36,864 -> 192 for `pre_us`, ~39,168 -> a handful for
`decode_us`) are the evidence; the actual ms improvement on real Ethos-U55/M55 silicon is the
bench-runner's to confirm on the next flash.

**HUD, `5V 0 mW SoM+LCD` every boot** (the isolated `feat/power-hud` branch read ~2 W standalone).
Audited every layer on this path -- `tr_rail5v_open()`/`tr_rail5v_poll()`'s call sites in `main.c`
(present, unconditional, not gated on `TR_INPUT_NPU`), the DT/pinctrl for `EVK_I2C_BUS_SENSORS`
(`== ALP_E1M_I2C0 == SoC I2C2`, confirmed `status = "okay"` in the generated dts, on pads P5_6/P5_7
-- NOT the I2C1 pads `i2c_handover_he.overlay` disables on the HE), the SDK's I2C handle pool (a fixed 4-slot pool,
not DT-derived, two concurrent opens on one `bus_id` are explicitly supported by the backend), and
the HUD's own read-through (`hud_l2.c` -> `hud.c`, unconditional, correct) -- **no logic bug found
anywhere on this path**. The one live difference from the isolated test is TIMING: `tr_rail5v_open()`
runs immediately after `tr_imu_open()`'s own BMI323 config on the SAME I2C2 bus. Mitigated with a
bounded retry (`src/platform/rail5v_power.c`, `RAIL5V_OPEN_RETRIES` = 5, matching `tr_display_open()`'s
own existing retry-for-a-plausible-power-up-race pattern in `main.c`) -- a real fix if this is a
back-to-back-open race, cheap if it is not. **Unresolved**: whether this actually fixes it can only
be confirmed on the next flash; if `5V 0 mW` persists, the next thing to check is a live I2C2
transcript (does `tr_imu_open()`'s LAST transaction complete cleanly before `tr_rail5v_open()`'s
first one starts).

**HUD, `M55-HP --`.** Root cause found: `hud_l2.c`'s `perf()` read the P10 SOUND ring's status word
(`TR_ARING_ADDR`), which `hp_vision` never writes (it is not the sound firmware) -- always reads as
"never set up". Fixed by giving `hp_dbg_t` (moved to a new shared header, `src/ipc/tr_hp_dbg.h`, so
both `hp_vision` and the HE's HUD have a canonical layout to agree on) two new CUMULATIVE-since-boot
fields, `busy_cyc`/`total_cyc` (real work: AE + letterbox + invoke + decode + thumbnail, NOT
capture-wait), the same shape as the HE's own `he_busy_cyc`/`he_all_cyc`; the HUD computes a load %
from the delta between two samples the identical way it already does for itself
(`src/hud/hud.c tr_perf_sample()`), falling back to the old sound-ring text when `hp_vision`'s
beacon magic is not seen valid across the whole window. `tests/host/test_hud.c` covers both the new
busy% path and the fallback.

**Frame-rate ramp (18 fps -> 30 fps over the first ~5 s, boots 2 and 3).** Not independently
measured this round (no hardware) -- the coordinator's own hypothesis (SRAM0/SRAM1 bandwidth
contention from HP init/pre-processing during the HE's own warm-up window) is plausible and
untested. The `pre_us`/`decode_us` fixes above cut the HP's own per-frame compute time
substantially (fewer divisions => less time the HP's bus master is issuing traffic against shared
SRAM0/SRAM1 per frame), which should SHRINK the contention window even if it does not eliminate
it -- this is a side effect of the perf fixes above, not a targeted fix for this item. No frame-skip
or pre-processing rate-limit was added: guessing at one without a way to measure whether it helps,
hurts, or merely masks the real cause was judged a worse bet than leaving the real hypothesis
documented for the next bench session, which can watch `TR_MEM_HP_DBG`'s `loop_hz_x10` alongside
the A32's own frame time during the first ~30 s after a cold boot and confirm (or rule out) the
contention theory directly.

**AE exposure/gain exposed in the beacon** (`hp_dbg_t.ae_exposure`/`ae_gain_idx`, mirrored every
frame): readable now without a live console.

## 13. Fix round 7: camera-liveness watchdog, HUD reads, live camera PiP

**Item 1, the real bug (silicon, 2c45a9c).** `capture_box()`'s own staleness gate (hp_state
RUNNING + pslot fresh) was never the thing `fall_back()`'s watchdogs (`calib_ticks`,
`paused_ticks`) counted -- they counted consecutive ticks of "no CALIBRATED player box", which
an EMPTY booth produces as reliably as a dead pipeline. Silicon proved it: the watchdog fired at
exactly `TR_CALIB_TIMEOUT_TICKS` with `hp_state=0 (RUNNING) stale=0` printed on the SAME line.
Fixed with a new liveness question, `tr_camera_ok()` (`g_pslot_stale==0 && hp_state==RUNNING`),
and `src/vision/camera_watchdog.h`'s `tr_watchdog_tick()` (reset-on-ok, else accumulate) -- an
empty booth with a healthy HP now resets the watchdog every tick and never times out.
Host-tested (`tests/host/test_camera_watchdog.c`; `main.c` itself is not host-testable).

**Item 2, `5V 0 mW` until ~t+30s.** `tr_rail5v_poll()` was only called inside the main game
loop, never during the pre-run title screen or calibration wait (both `TR_MODE_VISION`-only,
both take up to `TR_TITLE_MS` + `TR_CALIB_TIMEOUT_TICKS`). Added a poll call to both loops.

**Item 3, `M55-HP 0%` against a beacon reading ~99.5% busy.** Host-verified the arithmetic
itself is correct (`tr_perf_pct(787127318, 790822175)` rounds to 100, not 0), so the fix targets
the READ path rather than the formula: `hp_dbg_t.busy_cyc`/`total_cyc` had no seqlock at all
(unlike every other cross-core structure here) -- a torn 64-bit read on this 32-bit core was a
real, if not fully proven, candidate. Added `tr_hp_dbg_read_stable()` (double-read-until-stable,
needs no writer-side change since both fields are monotonic; host-tested,
`test_hp_dbg_stable_read.c`). Also closed a real UX ambiguity found along the way: the FIRST
window after the beacon goes valid has no real % yet (needs two consecutive valid samples) --
that window used to show a misleading "0%", indistinguishable from a genuinely idle HP; it now
shows "--" (`hp_pct_valid`, `test_hud.c` 3a3).

**Item 4, the frame-rate dip (10-20 fps for a few seconds at ~t+26-30s).** Not independently
reproduced -- item 1's own fix removes the exact trigger the silicon log showed (the fallback's
scene-rebuild transition at that same timestamp), so this is very likely the SAME event as item
1, not a separate SRAM bandwidth issue. Unconfirmed without a fresh flash.

**Item 5, live camera picture-in-picture.** New wire: `src/ipc/tr_cam_view.h/.c`, an HP -> A32
descriptor (`buf_addr` into `TR_MEM_CAM_POOL`, `frame_no`, `width`, `height`) at
`TR_MEM_CAM_VIEW`, same odd-before/even-after seqlock as `tr_pslot.h`. The PIXELS are never
copied through SRAM0 -- only the address of the latest complete `CAM_POOL` buffer is. Per the
design ruling, the buffer is NOT held past its existing release point (that would cost a frame
of NPU throughput every tick); a torn or mid-copy-recycled read is accepted as a cosmetic,
self-correcting risk (`tr_cam_view.h`'s own header note has the full reasoning), never a hard
error -- `tr_cam_view_still_seq()` is the second check a reader runs after copying pixel rows.

`src/render/cam_pip.h/.c`: the two pure, host-tested algorithms (`test_cam_pip.c`) -- a
nearest-sample GREY8->RGB565 row expansion (bit-exact: a grey pixel's R=G=B, so a 5/6/5
truncation is exact, not approximate), and pose-keypoint-to-PiP-space mapping (score-gated at
`TR_POSE_KP_MIN`, clamped in-bounds, same scale the row expansion uses).

`a32/renderer/render.c`'s `cam_pip_band()`: composites a 360x225 PiP (half the SCREEN's own
width, camera aspect kept) centred under the HUD banner, called per-band from `render_band()`
(only the 7-8 bands the PiP actually overlaps do any work), with a D-cache-line invalidate
(DCIMVAC) before every source row read (the camera writes CAM_POOL by DMA). Compile-time
switchable (`TR_CAM_PIP_ENABLE`, default 1). Cost is measured per-core per-frame
(`render_core_stats_t.cam_pip`, CNTVCT ticks) but not yet read back on silicon -- no hardware
this dispatch; the design's own <= 1 ms/core target is unverified.

`src/hud/hud.c`: a 7th perf-panel line, `"CAMERA 640x400 - NPU <Hz>"`, from `hp_dbg_t`'s own
`loop_hz_x10` -- grew `TR_PERF_LINES` 6->7 and `PERF_H` 120->138 (still inside the T_PERF tile's
own 140px bound). The logo/panel drawn at `PERF_Y+PERF_H+12` shifts down 18px with it;
NOT visually verified this round (no hardware, no way to screenshot the HE's real framebuffer).

**Scope cuts, disclosed rather than silently dropped (no hardware, real time budget):**
- **No NEON.** `tr_cam_pip_row_grey_to_rgb565()` is the scalar reference; a NEON-widened version
  must match it bit-for-bit before replacing it (`cam_pip.h`'s own comment). Not built this
  round -- correctness first, `arm_neon.h` intrinsics are easy to get subtly wrong without
  silicon to check against.
- **No skeleton bone lines**, dots only (`cam_pip_dot()`, a 3x3 filled square per confident
  keypoint). A clipped line rasteriser split across per-band calls needs more care than this
  round's remaining budget allowed; the mapping test (`test_cam_pip.c`) covers the geometry a
  future line-drawer would build on.
- **HUD panel layout (item 5's label + logo shift) is unverified on real glass.**

## 12. Fix round 6 -- peripheral ownership (silicon root cause of `5V 0 mW` AND `imu failed`)

**Root cause, both bugs at once.** Fix round 5's `5V 0 mW` fix (a bounded retry) treated a symptom
-- the real cause, proven on 2026W36-0009 with the HE halted, is that `hp_vision`'s board file left the
SoC's I2C2 (`0x49012000`) `status = "okay"` (the *base* board `.dts` turns it on unconditionally;
no hp_vision source ever asked for it), so Zephyr built and enabled an `i2c_dw_isr` for it on the
HP's own NVIC too. That controller is the SAME physical bus the HE's `imu.c` (BMI323) and
`rail5v_power.c` (INA236, U30) already share -- and the HP's ISR writes `INTR_MASK=0` on every
entry/exit regardless of which core's transaction owns it: `INTR_MASK 0x810` was proven to clear
within 50 ms of a cold boot with the HE core HALTED, i.e. purely from the HP's own traffic, and
polled SWD reads of U30 (~2.1 W, matching the isolated `feat/power-hud` figure) confirm the rail
itself was never the problem. `imu : BMI323 init failed -- tilt mode disabled` was the same bug
wearing a different symptom.

**Peripheral ownership table** (both cores; "owns" = the only node names either app's SOURCE asks
for -- everything else on a shared board `.dts` must be forced `disabled` in that app's own board
overlay, not left to whatever the base board file happens to default to):

| peripheral | M55-HP (`hp_vision`) | M55-HE (this game) | enforcement |
|---|---|---|---|
| I2C1 (`0x49011000`, camera SCCB) | **owns** (`csi_i2c`, ordering-fix shield) | disabled on the HE (`i2c_handover_he.overlay`, `TR_INPUT_NPU` builds; a shield whose bridge needs the bus enables it and the HE releases it at boot end, `alp,i2c-handover`) | `hp_vision_check.sh` i2c audit; the HP waits for the release |
| I2C2 (`0x49012000`, BMI323 + INA236) | **disabled** (fix round 6 -- was left `okay` by the base board file, the actual bug) | **owns** | `hp_vision_check.sh` i2c audit (new) |
| I2C0 (`0x49010000`) | **disabled** (fix round 6 -- unused by any hp_vision source, same audit) | n/a (HE base board leaves it as the board default; unrelated to this round's finding) | `hp_vision_check.sh` i2c audit (new) |
| CSI / CAM / D-PHY | **owns** (camera capture) | disabled by default (no camera shield attached) -- **except D-PHY**, which the display shield (`e1m_evk_rk055hdmipi4ma0`) also needs `okay` for its own DSI TX link; that is the HE's OWN dependency, not a camera leak, and stays on | build-time board default; confirmed via generated `zephyr.dts`, not asserted in code (no false leak to catch) |
| CDC200 / MIPI-DSI (display) | disabled by default (no display shield) | **owns** | build-time board default |
| Ethos-U55 | **owns** (256 MAC, HP-paired instance) | n/a (HE never opens `<alp/inference.h>`) | `hp_vision`'s own `&ethosu55 { status = "okay"; }` + `&ethosu85 { status = "disabled"; }` |
| UART / SPI / GPIO0-11,13,14 | disabled by default on both cores (neither app's source touches them) | disabled by default | build-time board default; confirmed via generated `zephyr.dts` |

**Update, `TR_HP_SOUND` builds (2026-10-08):** with the game sound linked into `hp_vision` the HP is no longer the "disabled" side of the I2C2 row. I2C2 (and GPIO5, I2S3, SPI1, LP-GPIO) are enabled in the HP image, I2C2 as `zephyr,deferred-init`, and the HE *leases* I2C2 + GPIO5 to the HP for the amp bring-up and takes them back (`src/ipc/tr_bus2.h`, `docs/2026-09-23-sound.md`). The table above is the plain image.

**Fix:** `hp_vision/boards/alp_e1m_aen803_m55_hp_ae822fa0e5597ls0_rtss_hp.overlay` now force-disables
`&i2c0` and `&i2c2`. **Enforcement, not just a one-time overlay edit:** `a32/release/hp_vision_check.sh`
(the same gate `build-release.sh` already refuses TR_HP_VISION=ON packaging without) now parses the
HP build's OWN generated `zephyr.dts` -- the definitive per-instance source of truth for what will
actually link, not an `nm` symbol match (one shared `i2c_dw_isr` function can't tell which instance(s)
it's wired to; the DTS `status` property can) -- and refuses to package if any `i2c@` node other than
`0x49011000` is `status = "okay"`. `tests/host/test_hp_vision_check.sh` covers both the pass case and
a synthetic i2c2-left-`okay` refusal, with a nested child-node fixture (`arx3a0@36`) reproducing the
exact bug this check's first draft had: a naive linear scan for the LAST `status` line inside an `i2c@`
node's braces matches the CHILD device's status, not the parent bus controller's -- the real i2c2 leak
was invisible to that naive version. The shipped check tracks brace depth instead.

**HE side:** already correct -- `i2c_handover_he.overlay` keeps I2C1 off on the HE; CSI/CAM are off by board
default (no camera shield); D-PHY's `okay` is the display's own DSI dependency, not a leak (see table).
No HE-side code or overlay change was needed for this fix.

**`camera unresponsive after 450 ticks` with the new HE (build6/build7) against the OLD HP (563e178).**
Not a pslot/beacon version mismatch -- `tr_pslot.h`'s own layout (`TR_PSLOT_MAGIC`/`TR_PSLOT_VERSION`,
unchanged this round) is unaffected. The actual cause: fix round 5 moved `TR_MEM_SRAM1_READY`
(`0x0237FCE0` -> `0x0237FCF0`) to make room for `hp_dbg_t`'s growth. Pairing a NEW HE (writes the ready
word to the new address) with the OLD, already-flashed HP (563e178, still polling the old address)
means the HP's `tr_sram1_ready()` never sees the flag: it sits forever in `TR_HP_STATE_SRAM1_NOT_READY`,
which is a valid, correctly-published `hp_state` -- just never `RUNNING`, so `capture_box()` never
returns a usable box and `fall_back()`'s 450-tick timeout is genuinely correct behaviour, one layer
removed from the real cause. **Fixed structurally, not just relocated again:** `TR_MEM_SRAM1_READY` now
sits at a FIXED address ahead of `TR_MEM_HP_DBG` (reserved 16 B, `0x0237FC90`), with `hp_dbg_t` given
~768 B of its own headroom after it (`0x0237FCA0`..`0x0237FFFF`) -- a future beacon growth spends that
budget and never needs to move the ready word again. **Also now detected, not just avoided:**
`fall_back()` (`src/main.c`) prints the pslot's last-accepted `hp_state` alongside the existing
"camera unresponsive" line, so this exact class of mismatch reads as "hp_state=4 (SRAM1_NOT_READY)"
directly in the HE console on the next occurrence, rather than requiring a memmap diff to diagnose.
The underlying lesson -- a NEW HE and an OLD HP are never a supported pairing across an IPC-layout
change -- is a flashing-procedure constraint (always flash both images from the same commit), not
something firmware can fully self-heal; the version/diagnostic improvements here shorten the time to
recognize a violation, not eliminate the need to avoid it.
