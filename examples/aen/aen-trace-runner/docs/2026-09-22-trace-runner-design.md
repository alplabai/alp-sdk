# Trace Runner — design

- **Date:** 2026-09-22
- **Status:** design, awaiting approval
- **Repo:** `trace-runner` (separate from `alp-sdk`; consumes the SDK's public API)
- **Target:** E1M-AEN803 SoM (Alif Ensemble E8, `AE822FA0E5597LS0`) on an E1M-EVK carrier,
  with the Rocktech RK055HDMIPI4MA0 720x1280 MIPI-DSI panel

## 1. Why this exists

One playable game that exercises the whole SoM at once, so a single demo answers
"what can this module do?" without a slide deck.

A port would not do that. Any of the well-known open-source ports proves the display
works and leaves the camera, the NPU, the 2D accelerator, the IMU and the second core
idle. Trace Runner is built around those blocks instead: every one of them does
something a viewer can see.

Audience, in priority order: engineers evaluating the SoM, then customers being pitched,
then a short video that travels.

## 2. What it is

An original endless runner in portrait orientation. The player runs along a circuit
board: copper traces are the lanes, components are the obstacles, solder blobs are the
pickups. All artwork is original and generated from simple geometry and small sprites
authored for this project. No third-party game content, no reference to any existing
title, character or franchise.

**The camera is the controller.** The player stands in front of the board and moves their
body: step left or right to change lane, jump to jump, crouch to duck. This is the primary
input, not a bonus mechanic. It also settles an ergonomic problem — someone standing in
front of the camera cannot reach the board to tilt it, so tilt cannot be the main control.

| Input | Role | Availability |
|---|---|---|
| Camera + NPU body tracking | **primary**: lane, jump, duck | camera works today; person-detection model exists |
| IMU tilt | **secondary mode** for handheld play, chosen in the menu | works today |
| Touch | menus, swipe controls | blocked: this panel's GT911 config block reads blank |
| BLE | two-board head-to-head race | later milestone |
| Audio | music and effects | EVK-03 only; not on the display board |

The two control modes are exclusive and chosen at the title screen: *stand up and play*, or
*hold it and tilt*. Neither is a fallback for the other mid-run, because switching control
scheme under the player would feel broken. If body tracking loses the player for longer
than a short grace period, the run pauses rather than silently handing over to tilt.

## 3. Non-goals

- Not a port, and not a clone of any existing game.
- No Linux dependency. The A32 track (section 9) is separate and must not block this.
- No network services. Wi-Fi association works on this hardware but DHCP does not, so
  nothing here depends on IP connectivity.
- No procedurally infinite content beyond what fits the asset budget; hand-authored
  segments shuffled at runtime is enough.

## 4. Hardware baseline

Measured on `E1M-AEN803` serial `2026W36-0009` unless noted.

| Block | State | Evidence |
|---|---|---|
| Display, 720x1280 RGB565 at 40 Hz | working | colour bars verified on glass, camera and by eye, 2026-09-22 |
| CDC200 layer 2 | available, unused | `enable-l2` / `pixel-fmt-l2` in `tes,cdc-2.1.yaml`, with its own framebuffer |
| GPU2D | API present | `alp_gpu2d_open/fill_rect/blit/blend/close` |
| Camera | first frames captured 2026-09-21 | OV9281 global-shutter mono shipped in #2247 |
| NPU (Ethos-U55/U85) | silicon-proven, dispatchable | `alp_inference_open()`; see `aen-npu-inference-person-mram` |
| IMU (BMI323, EVK U13) | working | carrier part, not on the SoM |
| SRAM0 | 4 MB; 2 MB framebuffer + 2 MB linker region | `sram0@2000000` |
| SRAM1 | 4 MB, entirely free | `sram1@2400000` |
| MRAM | 5.5 MB, holds the flashed image | `flash@80000000`, 5632 KiB |
| OSPI NOR (ISSI IS25WX256, 32 MB) | working on this unit | `cfg[0x00]=0xe7`, `cfg[0x07]=0xfd` |
| OSPI HyperRAM (64 MB) | blocked in hardware | CK/CK# crossed at U9; `SCPOL` does not reach the pads on AE822; needs the R3 fix |
| Touch (GT911) | blocked | config block `0x8047..0x8100` reads all `0x00`; upstream driver rejects it |
| Audio | not on this board | EVK-03 only |

Two consequences worth stating plainly: there is no external RAM, so the design lives
inside ~6 MB of SRAM; and the image plus assets exceed the 256 KB ITCM RAM-run, so this
project uses the MRAM slot0 flash flow from the start.

## 5. Architecture

### 5.1 Core split

- **M55-HE** — game loop, physics, rendering, display, IMU. Owns L1.
- **M55-HP** — camera capture and NPU inference. Produces a gesture verdict.
- The two communicate through a single-writer/single-reader mailbox in shared SRAM: the
  HP side writes `{sequence, gesture_id, confidence, timestamp}`, the HE side reads the
  latest. No locks, no blocking; a stale verdict simply ages out.

Rationale: the game must never stutter because inference was slow. Decoupling by one
small record means the worst case is a late gesture, not a dropped frame.

### 5.2 Display composition

- **L2**: the live camera feed, dimmed, as a backdrop. The player sees the board seeing
  them.
- **L1**: game sprites and HUD, alpha-blended over L2 by the CDC.
- Composition happens in the display controller. Neither layer copies the other, and the
  camera preview costs the game loop nothing.

### 5.3 Rendering

- Dirty-rect drawing, as proven in the arcade example: erase all, move all, draw all, in
  that order, every tick. Anything overdrawn by an erase is redrawn in the same tick.
- Sprite blits and alpha go through GPU2D; the CPU composes only what the accelerator
  cannot.
- Parallax comes from moving the layer window rather than redrawing pixels.
- Target 30 Hz logic against the panel's measured 40.0 Hz refresh.

### 5.4 Vision pipeline — the primary controller

Body tracking, not gesture classification. Where the player *is* is easier to detect
reliably than which of five poses they are striking, and it maps directly onto the game's
three controls.

- Camera frame -> downscale to the model's input -> `alp_inference_invoke()` -> a person
  bounding box -> a control record in the mailbox.
- **Lane** comes from the box's horizontal centre, split into three bands with hysteresis
  at the boundaries so a player standing on a line does not flicker between lanes.
- **Jump** is a sharp rise in the box's top edge; **duck** is a sharp drop in its height.
  Both are measured against a short rolling baseline of that player's own stance, so tall
  and short players behave the same.
- The model is the existing person detector that already runs from MRAM; a dedicated
  classifier is not needed for the first cut.
- Every record carries a confidence and a timestamp. If confidence stays low, or no person
  is found for longer than the grace period, the game pauses and says so on screen.

Calibration: the title screen doubles as calibration. The player is asked to step into
view and keep moving -- standing still would bake them into the detector's background
model and produce zero foreground for as long as they held it, so the prompt asks for the
opposite of what defeats calibration. The first box that clears confidence sets the stance
baseline and the lane band edges to that player's distance from the board.

### 5.5 Memory plan

The E8 is generous here, which is what makes this design comfortable rather than cramped.
Authoritative bank list for `AE822FA0E5597LS0`:

| Bank | Size |
|---|---|
| SRAM0 | 4096 KB |
| SRAM1 | 4096 KB |
| SRAM2 — M55-HP ITCM | 256 KB |
| SRAM3 — M55-HP DTCM | 1024 KB |
| SRAM4 — M55-HE ITCM | 256 KB |
| SRAM5 — M55-HE DTCM | 256 KB |
| **Total on-chip SRAM** | **~9.75 MB** |

(The "13.5 MB" figure in the family literature is the E7-class part, which adds SRAM6-9
but has a smaller SRAM1. The E8 instead carries two full 4 MB banks.)

Budget:

| Region | Contents | Size |
|---|---|---|
| MRAM 5.5 MB | application image, NPU model | flashed |
| SRAM0 4 MB | L1 game framebuffer, and a second buffer if double buffering wins | 1.84 MB each |
| SRAM1 4 MB | L2 camera framebuffer, game state, sprite working set, inference buffers | — |
| TCMs | hot loops and per-core working data | 1.75 MB total |
| NOR 32 MB | sprite atlases, level segments, audio later | streamed |

Two consequences: double buffering is affordable (3.7 MB of 8 MB), which removes tearing
as a concern; and the asset working set does not have to be trimmed to fit, because the
NOR holds far more than the game will use.

### 5.6 Modules

Each is independently testable, with no knowledge of the others' internals:

- `game/` — pure logic: lanes, obstacles, collisions, scoring. No hardware calls, so it
  runs and is unit-tested on the host.
- `render/` — turns game state into draw calls; owns the dirty-rect discipline.
- `input/` — merges tilt, gesture and (later) touch into one intent stream.
- `vision/` — camera plus inference on HP; its only output is the mailbox record.
- `assets/` — sprite and level loading from NOR.
- `platform/` — the only module that touches `<alp/*>`.

## 6. Milestones

Each one is a standalone demo; none leaves the board in a non-working state.

| # | Deliverable | Proves |
|---|---|---|
| M0 | Game core on the display, tilt steering (the secondary mode, built first because it needs no vision) | display, end to end |
| M1 | Camera feed live on L2 behind the game | camera + hardware composition |
| M2 | Person detection running, box drawn on screen, no gameplay effect | the NPU, and the tracking quality, before anything depends on it |
| M3 | **Body tracking drives the game** — lane, jump, duck | the headline: the camera is the controller |
| M4 | Camera + inference moved to HP, mailbox split | heterogeneous compute |
| M5 | GPU2D sprite path replacing CPU blits | the 2D accelerator |
| M6 | Assets streamed from NOR | external flash |
| M7 | Touch menus | needs the GT911 config resolved first |
| M8 | Audio on EVK-03, BLE two-board race | audio + radio |

M2 before M3 is deliberate: the tracking is shown on screen and judged by eye while it
still cannot break the game. Only once it looks solid does it take the controls.

M0 reuses the existing tilt-arcade example as its starting point rather than starting from
a blank file.

## 7. Risks and spikes

| Risk | Why it matters | First move |
|---|---|---|
| Body-tracking cost and quality | the primary controller depends on it | spike first: per-frame cost on the U55, and whether the camera's field of view covers a standing player at a usable distance |
| Camera placement | the board sits on a desk; a standing player may be out of frame | part of the same spike; may force a stand, a wider lens, or a seated-play framing |
| Frame budget | full-screen redraw at 1.84 MB is far too slow | dirty rects plus GPU2D, measured at M0 and again at M2 |
| NOR read path | no in-tree device transfer exists; the flash driver ships no `flash_driver_api` | spike before M5; MRAM alone is enough until then |
| Panel init defect | about 1 boot in 8-10 fails to init, no recovery path | documented; the demo needs a power-cycle habit until it's fixed |
| Touch | the controller's config block is blank | M6 is gated on it; everything before that ignores touch |
| Camera framing | a desk-mounted board sees a narrow slice of a standing player | resolve during the gesture spike |

## 8. Testing

- `game/` is host-compiled and unit-tested: collisions, scoring, spawn logic, and the
  erase/draw invariant as a pure-state assertion.
- Every milestone gets a bench run on real silicon with a cold power cycle, and the
  optical result is recorded by camera rather than inferred from the console.
- A build that only links is not evidence. Each milestone's claim is the thing observed on
  the glass.

## 9. What this demo proves about what we ship

Alp Lab sells the module. A customer who buys an E1M-AEN803 expects the display, camera,
NPU, IMU and radio to be usable from a supported software stack, on both the M55 and the
A32 side. So this game is not only marketing: it is a customer-shaped consumer of our own
SDK, and the friction it hits is the friction a customer hits.

Two rules follow, and they matter more than the game:

1. **Everything the game needs goes through the public `<alp/*>` API.** If a capability is
   awkward or missing there, that is a finding against the SDK, logged and fixed in the
   SDK — not worked around inside the game. The list of such findings is a deliverable of
   this project in its own right.
2. **Anything the game cannot do because a driver does not exist is a product gap**, and
   gets an issue on the SoM roadmap rather than a workaround here.

Known gaps this project will expose, all of them things we owe customers:

| Gap | Status today | Consequence for a customer |
|---|---|---|
| A32 Linux display | no DRM driver for CDC200 + DesignWare DSI; the `hx8394` panel patch we carry targets the Renesas V2N layer and cannot scan out alone | a Linux customer has no screen |
| A32 Linux camera / NPU | V4L2 and the Ethos-U Linux stack not plumbed | no vision or inference under Linux |
| A32 Yocto machines | `e1m-aen803-a32` machine build failures (#1968) | cannot build an image at all |
| Touch | the panel's GT911 config block reads blank; the upstream driver refuses to bind | no touch on a touch panel |
| HyperRAM | CK/CK# crossed at U9; no firmware lever, needs the R3 board fix | 64 MB of advertised RAM unusable |
| Panel init | fails roughly 1 cold boot in 8-10, with no re-init path | a display that sometimes does not come up |
| OSPI device access | no in-tree transfer path; the flash driver ships no `flash_driver_api` | the 32 MB NOR is hard to use |

### The A32 Linux stack

Linux 6.12.6 boots to a shell on this SoM's Cortex-A32 (2026-09-05, TF-A BL32, MRAM-XIP,
no U-Boot). Bringing its graphics, camera and NPU support up is a **product deliverable**,
not an optional extra, and it is the right home for any mature engine port later. It is
tracked as its own project so the two efforts do not block each other, but its priority
comes from the customer promise, not from this game.

The demo is built on the M55 pair because that stack works end to end today, which lets
the Linux gap be closed on its own schedule instead of under demo pressure.

## 10. Licensing

- This repo is Apache-2.0, matching the SDK, and consumes `alp-sdk` through its public
  API only.
- All game content is original to this project.
- No GPL-licensed engine is vendored here. Any such port belongs in its own repository
  under its own licence.

## 11. Open questions

1. Gesture set: five classes as listed, or fewer for the first cut?
2. Does the demo need to survive a stranger playing it unattended, which would raise the
   priority of the panel-init defect and of touch?
3. Is a two-board BLE race worth the second EVK, or is a single-board demo enough?
