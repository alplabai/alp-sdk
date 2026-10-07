# aen-trace-runner

Trace Runner is an endless-runner showcase game for the E1M-AEN803 SoM (Alif Ensemble E8)
on the E1M-EVK carrier with the RK055HDMIPI4MA0 720x1280 MIPI-DSI panel. It is the demo Alp
Lab runs at exhibitions: display, camera, NPU, IMU and three of the E8's cores all do visible
work at once. The player steers by moving their body in front of the camera. MoveNet pose
estimation runs on the M55-HP's Ethos-U55, the game logic and HUD run on the M55-HE, and a
bare-metal Cortex-A32 renders the 3D scene.

**Bench status:** on 2026-09-29 the release was built from this directory (HE + HP vision + A32
renderer), packaged with `a32/release/build-release.sh` and flashed with
`a32/release/flash-release.sh` onto an E1M-AEN803 (2026W36-0009) on the E1M-EVK. After each of
three cold power cycles it booted standalone into the game, running 30.0 fps (300 panel flips in
10 s) with 0 dropped frames. The A32 image built here was byte-identical to the one already
resident on that unit.

## Core split

| Core | Image | Job |
| --- | --- | --- |
| M55-HE | this directory (`CMakeLists.txt`, `src/`) | Game state, input, HUD (CDC200 layer 2), frame pacing, A32 watchdog |
| M55-HP | `hp_vision/` | OV9281 camera, MoveNet on the Ethos-U55, publishes a pose slot in SRAM0 |
| Cortex-A32 | `a32/stub` + `a32/renderer` | Bare-metal 3D renderer (NEON), booted by TF-A, renders into the panel framebuffer |

`docs/2026-09-22-core-allocation.md` has the reasoning. `docs/2026-09-22-trace-runner-design.md`
covers the game design, and `docs/superpowers/` holds the implementation plans.

## What you need

- An E1M-AEN803 on an E1M-EVK, with the RK055HDMIPI4MA0 panel on the display connector and an
  InnoMaker CAM-OV9281 on J5.
- The Zephyr SDK (`arm-zephyr-eabi`) plus the `tflite-micro` Zephyr module (for `hp_vision`).
- Arm GNU Toolchain `arm-none-eabi` 13.x for the A32 images.
- Alif SETOOLS (`app-release-exec-linux`), plus the TF-A `bl32.bin`, `m55_stub_hp.bin` and
  `app-device-config.json` for an A32 ATOC in its `build/images` and `build/config`. These come
  from Alif; this example does not ship them.
- For the pose model: MoveNet SinglePose Lightning int8 v4 (Kaggle
  `google/movenet/tfLite/singlepose-lightning-tflite-int8`) and `ethos-u-vela`.

## Build

The game can also run without the A32 and without the HP: `-DTR_RENDER=M55` renders on the HE
itself, and tilting the board (IMU) steers. The steps below build the full exhibition release.

1. Build the A32 stub and renderer, then generate the header the HE needs to launch them. The
   A32 images are built with GNU make; on Windows, run this step in WSL2.

<!-- cross-platform-lint:ignore -->
   ```sh
   make -C a32/stub
   make -C a32/renderer
   cp a32/renderer/renderer-launch.h build/tr_launch.h
   ```
<!-- cross-platform-lint:resume -->

2. Build the M55-HE game in A32 mode, with auto-launch, the 30 Hz panel timing and pose input
   from the HP:

   ```sh
   west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d build/he . -- \
     -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H=$PWD/build/tr_launch.h \
     -DTR_PANEL_HZ=30 -DTR_INPUT_NPU=ON -DTR_CAM_ROTATE=90
   ```

   `TR_CAM_ROTATE` is the camera's mounting rotation, clockwise, as seen on the panel: `0`, `90`
   or `270`. `90` matches the reference unit.

3. Build the M55-HP vision image:

   ```sh
   west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp -d build/hp hp_vision -- \
     -DTR_CAM_ROTATE=90 -DTR_CAM_MIRROR=ON
   ```

4. Cut MoveNet down to its NPU body and compile it with Vela. `tools/movenet_cut.py`'s header
   has the exact commands, and `probe/npu/README.md` has the Vela invocation.

5. Package the release ATOC and the Flow D inputs, working in a private copy of SETOOLS:

   ```sh
   ST=$(mktemp -d)/setools && cp -a "$SETOOLS_DIR" "$ST"
   TR_HP_VISION=ON TR_HP_VISION_BUILD=build/hp TR_HP_VISION_MODEL=<movenet_cut_vela.tflite> \
     bash a32/release/build-release.sh "$ST" build/he
   ```

   `build-release.sh` refuses to package an HE image whose launch header doesn't match the
   renderer it packages. It also refuses one that was not built with `TR_PANEL_HZ=30`, or an HP
   build without an explicit camera rotation.

## Flash

`a32/release/FLASH-RECIPE.md` is the ordered procedure, and `a32/release/flash-release.sh`
implements it. It writes MRAM over SWD with a J-Link, using this repository's
`scripts/bench/aen/bench-env.sh`.

**Before anything else, take a full MRAM read-back** (`flash-release.sh readback`). The flash
rewrites whole 16 KiB sectors, and that read-back is the only way to restore the board.

## Options

| CMake option | Default | Effect |
| --- | --- | --- |
| `TR_PANEL` | `rk055` | `rvt121` targets the Riverdi 12.1" LVDS panel, A32 render only (see below) |
| `TR_RENDER` | `A32` | `M55` renders on the HE instead (no A32 needed) |
| `TR_INPUT_NPU` | `OFF` | Read the player's pose from the HP's pose slot |
| `TR_CAMERA` | `OFF` | HE reads the camera itself (needs `TR_RENDER=M55`) |
| `TR_PANEL_HZ` | `40` | `30` for the release panel timing (A32 render only; with `TR_PANEL=rvt121` it must be `30`) |
| `TR_PANEL_ROTATE` | `90` | `TR_PANEL=rvt121` only: `90` or `270`, the direction the portrait frame is turned onto the panel |
| `TR_M55_AUTOLAUNCH` | `OFF` | HE launches the A32 renderer at boot (release) |
| `TR_TILT_TAKEOVER` | `OFF` | The IMU tilt takes over steering when no player is seen |

## Riverdi RVT121 (12.1" LVDS)

`-DTR_PANEL=rvt121` runs the game on the Riverdi RVT121HVDFWCA0-B, 1280x800 landscape RGB565,
through the SN65DSI83 DSI-to-LVDS bridge adapter (shield `e1m_evk_rvt121hvdfwca0`). The shield's
bridge driver brings the panel up, so the HX8394 retry in `src/platform/panel.c` is a no-op, and
none of `panel_deferred.overlay`, `panel_30hz.overlay` or `i2c1_off.overlay` is applied (the
bridge and the touch controller live on I2C1). The panel runs at its native ~30.06 Hz
(36.363636 MHz pixel clock, 1440x840 totals), hence `TR_PANEL_HZ=30`.

The game is the same A32-rendered 720x1280 portrait game as on the RK055, rotated when it is
written to the scan-out buffer (`src/render/panel_rot.h`, the one definition of the mapping).
The panel is mounted on its side, so:

- Layer 1 is a 1280x720 window centred on the 800 rows (`panel_rvt121_window.overlay`: rows
  40..759, black bars above and below). 1280 x 720 x 2 B is the portrait frame's size exactly, so
  no address in `src/ipc/tr_memmap.h` moves and the memory map is the RK055's.
- The A32 renderer rotates each 32-row band as it copies it out to the framebuffer (NEON 8x8
  transposes, whole 16-byte stores), the video half included.
- The HUD (layer 2) becomes a 352x720 window at the panel edge the portrait top lands on, 40 rows
  down, in the same buffer; `src/hud/hud.c` writes it rotated.
- `TR_PANEL_ROTATE` is `90` (clockwise: the portrait top lands on the panel's right edge, the
  default) or `270` (anticlockwise, the left edge). Pick the one that reads upright on the
  mounted panel. The HE and the A32 renderer must be built with the same value.

<!-- cross-platform-lint:ignore -->
```sh
make -C a32/stub
make -C a32/renderer TR_PANEL_ROTATE=90   # also writes renderer-launch.h, rotation included
cp a32/renderer/renderer-launch.h build/tr_launch.h
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d build/he . -- \
  -DTR_PANEL=rvt121 -DTR_PANEL_HZ=30 -DTR_PANEL_ROTATE=90 -DTR_INPUT_NPU=ON -DTR_CAM_ROTATE=90 \
  -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H=$PWD/build/tr_launch.h
west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp -d build/hp hp_vision -- \
  -DTR_PANEL=rvt121 -DTR_CAM_ROTATE=90 -DTR_CAM_MIRROR=ON
```
<!-- cross-platform-lint:resume -->

Wiring: bridge EN = `CK_INT` (P13_4), backlight = `CK_PWM0` (P10_7), and P9 powers +1V8.  The backlight is a 30% duty, 500 Hz UTIMER3 PWM set once at boot by `tr_panel_up()`.

I2C1 is shared. The shield's SN65DSI83 bridge is configured over I2C1 by the HE at boot, and with
`TR_INPUT_NPU` the HP owns the same controller for the camera. After the bridge is up
(`tr_panel_up()`), `src/platform/i2c1_handoff.c` masks the HE's I2C1 interrupt, stops its
controller and writes `TR_I2C1_FREE_MAGIC` to `TR_MEM_I2C1_FREE` (`0x0237FC94`, SRAM0, the
reserved block next to `TR_MEM_SRAM1_READY`; `src/ipc/tr_i2c1_flag.h`). The HP image built with
`-DTR_PANEL=rvt121` waits for that word before it unsticks the pads or initialises the bus and
the camera, reports every 20 s on its console while it is missing, and never touches the bus
until it arrives. The HE clears the word first thing at every boot, and the touch controller is
disabled, so nothing on the HE uses I2C1 afterwards. Start the HE first, then the HP; the HP
build for RK055 does not wait. An HE-only reset while the HP is streaming would reconfigure the
bridge on a bus the HP is using, so reset both.

Limits: `TR_RENDER=A32` only (the M55 2D renderer has no rotation), and CMake refuses
`TR_CAMERA`. `TR_CAM_ROTATE` is the camera's mounting rotation and is independent of
`TR_PANEL_ROTATE`; the camera view and skeleton are drawn in the portrait video half and rotated
with the rest of the frame. Touch is not used. The sound image is unaffected (it uses I2C bus 0
and I2S3, not I2C1). Build-only: the full game on this panel is not yet verified on hardware.

## Sound (reworked carriers only)

`sound/` is an M55-HP image that plays the game's music and effects through the EVK's two
TAS2563 amplifiers on I2S3. **It refuses to build unless you pass `-DTR_SND_REWORKED_U46=ON`,
and only a carrier whose U46 has been reworked may run it.** On a stock 2626-R2 carrier, U46 is
a 74LVC157 with no high-impedance state, and it contends with the SoC the moment I2S3 is muxed
onto P9_3/P9_4/P9_5. `a32/release/sound-carriers.txt` is the per-unit allow list the release
script checks. Sound also needs a Zephyr tree that sets the 76.8 MHz audio clock; see
`docs/2026-09-23-sound.md`. The exhibition release runs `hp_vision` on the HP instead, without
sound.

## Tests

```sh
bash tests/host/runner.sh
```

The runner builds and runs the host unit tests (game logic, vision post-processing, renderer
goldens, the release interlocks). When `arm-none-eabi-gcc` and `qemu-arm` are on `PATH`, it also
runs the renderer tests under A32 NEON and compile-checks the M55 sources.

## Known SDK gaps

- `<alp/display.h>` has no page-flip operation. `src/platform/display.c` calls the CDC200
  driver's own `cdc200_swap_fb()` through its private header (see `CMakeLists.txt`).

## Licence

Apache-2.0, copyright Alp Lab AB. All game art, meshes and music are original to this project.
The HUD font is rendered at build time from DejaVu Sans Bold (Bitstream Vera licence).
