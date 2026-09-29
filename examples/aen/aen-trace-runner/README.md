# aen-trace-runner

Trace Runner is an endless-runner showcase game for the E1M-AEN803 SoM (Alif Ensemble E8)
on the E1M-EVK carrier with the RK055HDMIPI4MA0 720x1280 MIPI-DSI panel. It is the demo Alp
Lab runs at exhibitions: display, camera, NPU, IMU and three of the E8's cores all do visible
work at once. The player steers by moving their body in front of the camera. MoveNet pose
estimation runs on the M55-HP's Ethos-U55, the game logic and HUD run on the M55-HE, and a
bare-metal Cortex-A32 renders the 3D scene.

**Bench status:** the release built from these sources (HE + HP vision + A32 renderer) is
resident on an E1M-AEN803 (2026W36-0009) on the E1M-EVK. After a cold power cycle on 2026-09-25
it booted standalone into attract mode and measured 30.0 fps with 0 dropped frames.

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

1. Build the A32 stub and renderer, then generate the header the HE needs to launch them:

   ```sh
   make -C a32/stub
   make -C a32/renderer
   python3 a32/stub/mkpayload.py info a32/renderer/renderer.bin --c-header build/tr_launch.h
   ```

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
| `TR_RENDER` | `A32` | `M55` renders on the HE instead (no A32 needed) |
| `TR_INPUT_NPU` | `OFF` | Read the player's pose from the HP's pose slot |
| `TR_CAMERA` | `OFF` | HE reads the camera itself (needs `TR_RENDER=M55`) |
| `TR_PANEL_HZ` | `40` | `30` for the release panel timing (A32 render only) |
| `TR_M55_AUTOLAUNCH` | `OFF` | HE launches the A32 renderer at boot (release) |
| `TR_TILT_TAKEOVER` | `OFF` | The IMU tilt takes over steering when no player is seen |

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
