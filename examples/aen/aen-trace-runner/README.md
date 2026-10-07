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
   python3 a32/stub/mkpayload.py info a32/renderer/renderer.bin --c-header build/tr_launch.h
   ```
<!-- cross-platform-lint:resume -->

2. Build the M55-HE game in A32 mode, with auto-launch, the 30 Hz panel timing and pose input
   from the HP (`panel_30hz.overlay` stretches the RK055 shield to 30 Hz; the display itself is
   `-DSHIELD=`, the RK055 by default):

   ```sh
   west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d build/he . -- \
     -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H=$PWD/build/tr_launch.h \
     -DEXTRA_DTC_OVERLAY_FILE=$PWD/panel_30hz.overlay -DTR_INPUT_NPU=ON -DTR_CAM_ROTATE=90
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
   renderer it packages. It also refuses one whose display does not refresh at 30 Hz (read from its `zephyr.dts`), or an HP
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
| `SHIELD` | `e1m_evk_rk055hdmipi4ma0` | The display. `e1m_evk_rvt121hvdfwca0` is the Riverdi 12.1" LVDS panel (see below). Nothing else in the build names a panel |
| `TR_RENDER` | `A32` | `M55` renders on the HE instead (no A32 needed) |
| `TR_INPUT_NPU` | `OFF` | Read the player's pose from the HP's pose slot |
| `TR_CAMERA` | `OFF` | HE reads the camera itself (needs `TR_RENDER=M55`) |
| `TR_M55_AUTOLAUNCH` | `OFF` | HE launches the A32 renderer at boot (release) |
| `TR_TILT_TAKEOVER` | `OFF` | The IMU tilt takes over steering when no player is seen |

## Switching the display: Riverdi RVT121 (12.1" LVDS)

The game does not know which panel it is on. A display is a shield (`-DSHIELD=`), and everything
the game needs comes from that shield's devicetree through the SDK's display API:

- the refresh (`pclk / (htotal * vtotal)` of the `cdc200` node, `src/game/panel_hz.h`): 40 Hz for
  the RK055, 30 Hz for the RVT121, 30 Hz for the RK055 with `panel_30hz.overlay`;
- the geometry and the panel's `mount-rotation` (`alp_display_caps_t.rotation`): the game stays
  720x1280 portrait and turns the frame by that many degrees clockwise when it writes the scan-out
  buffer (`src/render/panel_rot.h`, the one definition of the mapping). The RVT121 is mounted on
  its side and declares 90;
- the backlight (`alp,display-backlight` in the shield: the RVT121's 30% PWM; the RK055's HX8394
  owns its own enable);
- the panel bring-up (the SDK's `panel_init_retry.c` for the HX8394, the SN65DSI83 driver for the
  RVT121), both before `main()`.

```sh
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d build/he . -- -DSHIELD=e1m_evk_rvt121hvdfwca0
```

The only per-shield file in the game is `shield-fit/<shield>.overlay`, applied automatically when
it exists: the RVT121's fits the portrait content to a 1280x720 layer-1 window centred on its 800
rows (black bars above and below), which is the portrait frame's byte count exactly, so no address
in `src/ipc/tr_memmap.h` moves. The A32 renderer rotates each 32-row band as it copies it out
(NEON 8x8 transposes), the video half included; the HUD (layer 2) becomes a 352x720 window at the
edge the portrait top lands on, in the same buffer. The renderer is ONE binary for every display:
the HE puts the rotation in every frame (mailbox version 2), and the renderer faults on a version
or rotation it cannot produce rather than draw something else.

I2C1 is shared. The RVT121's SN65DSI83 bridge is configured over I2C1 by the HE at boot, and with
`TR_INPUT_NPU` the HP owns the same controller for the camera. Every `TR_INPUT_NPU` system carries
an `alp,i2c-handover` node on each side (`i2c_handover_he.overlay`, `hp_vision/boards/*.overlay`;
SDK glue in `zephyr/soc-bridge/alif/i2c_handover.c`): at the end of its boot the HE masks its I2C1
interrupt, stops its controller and writes a flag word (`TR_MEM_I2C1_HANDOVER`, `0x0237FC94`,
SRAM0); the HP waits for it before it unsticks the pads or initialises the bus and the camera,
reports on its console every 20 s while it is missing, and clears it once it has it. Start the HE
first, then the HP. An HP restarted alone after it took the bus waits for a release that is not
coming: restart both, or write `0x31433249` to the flag word first. The touch controller is
disabled in the fit overlay (the game does not use it). `TR_CAM_ROTATE` is the camera's mounting
rotation and is independent of the panel's; the camera view and skeleton are drawn in the portrait
video half and rotated with the rest of the frame.

Limits: the 2D `TR_RENDER=M55` path cannot rotate and refuses a display with a `mount-rotation` at
build time; `TR_CAMERA` is refused with `TR_RENDER=A32`. The sound image is unaffected (it uses I2C
bus 0 and I2S3, not I2C1). Build-only for the RVT121 shield flow until it is run on the bench.

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
