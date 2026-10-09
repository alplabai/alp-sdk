# aen-trace-runner

Trace Runner is an endless-runner showcase game for the E1M-AEN803 SoM (Alif Ensemble E8)
on the E1M-EVK carrier with the RK055HDMIPI4MA0 720x1280 MIPI-DSI panel. It is the demo Alp
Lab runs at exhibitions: display, camera, NPU, IMU and three of the E8's cores all do visible
work at once. The player plays with their arms and body in front of the camera. MoveNet pose
estimation runs on the M55-HP's Ethos-U55, the game logic and HUD run on the M55-HE, and a
bare-metal Cortex-A32 renders the 3D scene.

**Bench status:** on 2026-09-29 the release was built from this directory (HE + HP vision + A32
renderer), packaged with `a32/release/build-release.sh` and flashed with
`a32/release/flash-release.sh` onto an E1M-AEN803 (2026W36-0009) on the E1M-EVK. After each of
three cold power cycles it booted standalone into the game, running 30.0 fps (300 panel flips in
10 s) with 0 dropped frames. The A32 image built here was byte-identical to the one already
resident on that unit.

## Controls

The player stands in front of the camera and plays with their arms. Left and right are the
PLAYER's own, as they see themselves on the screen (a selfie view).

| To | Do | What the game reads |
| --- | --- | --- |
| Move one lane left | Raise your **left arm** | The left wrist clearly above the left shoulder |
| Move one lane right | Raise your **right arm** | The right wrist clearly above the right shoulder |
| Jump | Raise **both arms** together | Both wrists up within `TR_ARM_SETTLE_POSES` of each other |
| Duck | Crouch | The torso centre drops against your own baseline |

- **One raise, one step.** Each arm fires once when it goes up. To step again, lower the arm
  and raise it again. Holding an arm up does nothing more.
- **"Raised"** means the wrist is more than `TR_ARM_UP_PCT` (50 %) of your shoulder width above
  the same-side shoulder, so the gesture is the same at any distance from the camera. The arm
  re-arms only once it is below `TR_ARM_DOWN_PCT` (20 %); the gap between the two is the
  hysteresis. A wrist or shoulder the model is not sure of (below `TR_ARM_KP_MIN`, 77 of 255) is
  ignored, and so is a player turned side-on (shoulders closer than `TR_ARM_MIN_SHOULDER_PX`).
- **A both-arms raise never steps a lane.** A lane step waits `TR_ARM_SETTLE_POSES` (4 poses,
  about 100 ms, counted on every pose) for the other arm; if it comes up inside that window the
  gesture is a jump and both arms are spent. The price is that a lane step lands about 100 ms
  after the arm crosses the line. A wrist that leaves the frame or sinks back into the hysteresis
  band during the window never confirms its rise. If the second arm comes up later, while the
  first is still up from its own lane step, that is a jump too.
- **A jump is not lost.** Asked during a duck or the last steps of a jump, it is held
  `TR_JUMP_BUFFER_TICKS` (4) game steps and taken as soon as the runner is free.
- Where you stand is not a control: stepping sideways does nothing. An arm already up when you
  join, or when play resumes after a pause, has to be lowered before it counts.
- **Left and right follow the camera.** The pose model labels a limb by the anatomy it sees, so
  `src/vision/pose.c` does not trust its left/right labels: the shoulder with the smaller x is on
  the screen's left, and `TR_CAM_MIRROR` says whose arm that is (the player's left in a selfie
  view, their right if the camera is not mirrored). The pose is already in the upright frame, so
  the rule is the same at every `TR_CAM_ROTATE`. `tests/host/test_cam_mirror.c` checks it through
  the sensor flip and the rotation for `0`, `90` and `270`, and `tests/host/test_arms.c` with the
  mirror on and off. On the HE the mirror is not taken from its own `TR_CAM_MIRROR` but from the
  HP's camera descriptor (`tr_cam_view_t.mirror`, the sensor flip bit read back), so what the sensor
  really does decides; the build's value is only the fallback before the HP has published and the
  `!!!!!` warning printed once if the two disagree. The rotation-0 flip bit has not been seen on
  glass yet: `a32/release/FLASH-RECIPE.md` has the bench acceptance step and the
  `TR_OV9281_HMIRROR_ACTIVE_LOW` HP option if it turns out reversed.
- The lamps on the plate along the bottom edge of the camera picture say it on screen: **LEFT ARM**,
  **RIGHT ARM**, **BOTH ARMS** and **DUCK** light as the game acts on them. The picture itself is
  clear of overlays at the top, where the arms go up.
- Tilting the board (IMU) is still the camera-less way to play; see `TR_TILT_TAKEOVER`.

The thresholds are named constants in `src/vision/arms.h` (arms) and `src/vision/track.h` (duck).

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
   make -C a32/renderer TR_CAM_ROTATE=0
   python3 a32/stub/mkpayload.py info a32/renderer/renderer.bin --c-header build/tr_launch.h
   ```
<!-- cross-platform-lint:resume -->

2. Build the M55-HE game in A32 mode, with auto-launch, the 30 Hz panel timing and pose input
   from the HP (`panel_30hz.overlay` stretches the RK055 shield to 30 Hz; the display itself is
   `-DSHIELD=`, the RK055 by default):

   ```sh
   west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d build/he . -- \
     -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H=$PWD/build/tr_launch.h \
     -DEXTRA_DTC_OVERLAY_FILE=$PWD/panel_30hz.overlay -DTR_INPUT_NPU=ON -DTR_CAM_ROTATE=0 -DTR_CAM_MIRROR=ON
   ```

   `TR_CAM_ROTATE` is the camera's mounting rotation, clockwise, as seen on the panel: `0`, `90`
   or `270`. `0` is the camera mounted upright and shown LANDSCAPE: that is the E1M-EVK bench
   release (E1M-EVK, the OV9281 on the RPi CSI connector), and what the arm controls want, since
   arms reach sideways. Only `0` is drawn on the panel (scaled up to fill the camera area); a camera
   the HP turns (`90` / `270`, a camera mounted on its side) still drives the game, but the video
   area says `ROT nn` instead of showing it sideways. `90` is the default and matches the first
   reference unit. `TR_CAM_MIRROR` (default `ON`) says the HP mirrors the
   view like a selfie; the arm controls read which arm is the player's left from it, so set it
   exactly as the HP build. `build-release.sh` refuses an HE and HP pair that disagree on
   `TR_CAM_MIRROR`, or on whether `TR_CAM_ROTATE` is `0`.

3. Build the M55-HP vision image:

   ```sh
   west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp -d build/hp hp_vision -- \
     -DTR_CAM_ROTATE=0 -DTR_CAM_MIRROR=ON
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

Build the whole release on ONE machine with ONE `arm-none-eabi` toolchain: the renderer, the launch
header made from that exact `renderer.bin`, and the HE. The same sources give different
`renderer.bin` bytes (a different length and CRC) under different gcc packages (Debian's
`gcc-arm-none-eabi` 13.2.rel1-2 and Arm's 13.2.rel1 tarball differ by 265 bytes), and an HE whose
launch header came from another build's renderer is refused by `build-release.sh`.

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
| `TR_CAM_ROTATE` | `90` | With `TR_INPUT_NPU`: the camera's mounting rotation, `0` (landscape, the EVK bench release), `90` or `270` (portrait). Same value on the HP build |
| `TR_CAM_MIRROR` | `ON` | With `TR_INPUT_NPU`: the HP mirrors the view like a selfie; decides which arm is the player's left. Same value on the HP build |
| `TR_M55_AUTOLAUNCH` | `OFF` | HE launches the A32 renderer at boot (release) |
| `TR_TILT_TAKEOVER` | `OFF` | The IMU tilt takes over steering when no player is seen |

## Switching the display: Riverdi RVT121 (12.1" LVDS)

The game does not know which panel it is on. A display is a shield (`-DSHIELD=`), and everything
the game needs comes from that shield's devicetree through the SDK's display API:

- the refresh (`pclk / (htotal * vtotal)` of the `cdc200` node, `src/game/panel_hz.h`): 40 Hz for
  the RK055, 30 Hz for the RVT121, 30 Hz for the RK055 with `panel_30hz.overlay`;
- the geometry and the panel's `mount-rotation` (`alp_display_caps_t.rotation`): the game stays
  portrait and turns the frame by that many degrees clockwise when it writes the scan-out
  buffer (`src/render/panel_rot.h`, the one definition of the mapping). The RVT121 is mounted on
  its side and declares 90. The picture is rendered natively 800 columns wide (`TR_R3D_W`), 768
  rows of game over 512 of camera, and the frame is written `fw` columns wide: the panel's width,
  taken from its window (`tr_frame_in_t.fw`, mailbox version 4). The RVT121 shows all 800; the
  RK055 (720) shows the centre 720, cropped, never scaled;
- the backlight (`alp,display-backlight` in the shield: the RVT121's 30% PWM; the RK055's HX8394
  owns its own enable);
- the panel bring-up (the SDK's `panel_init_retry.c` for the HX8394, the SN65DSI83 driver for the
  RVT121), both before `main()`.

```sh
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he -d build/he . -- -DSHIELD=e1m_evk_rvt121hvdfwca0
```

The only per-shield file in the game is `shield-fit/<shield>.overlay`, applied automatically when
it exists: the RVT121's only disables the touch controller and enables I2C1 (the layer-1 window is
the shield's whole 1280x800, which is 2,048,000 B, exactly one framebuffer slot, so no address in
`src/ipc/tr_memmap.h` moves). The HE takes `fw` from the CDC200 driver's `fb_size` and checks it
against the devicetree window, because `cdc200_swap_fb()` rejects any other size. The A32 renderer rotates each 32-row band as it copies it out
(NEON 8x8 transposes), the video half included; the HUD (layer 2) is placed at open from the
rotation and the layer-1 window, 352x720 at the edge the portrait top lands on and centred along
it (`tr_hud_window_off()`), in the same buffer. It stays 720 wide on every panel. The renderer is ONE binary for every display: the HE puts the rotation (0, 90 or 270; a
16-bit field) in every frame (mailbox version 2), and the renderer faults on a version or rotation
it cannot produce rather than draw something else.

I2C1 is shared. The RVT121's SN65DSI83 bridge is configured over I2C1 by the HE at boot, and with
`TR_INPUT_NPU` the HP owns the same controller for the camera. Every `TR_INPUT_NPU` system carries
an `alp,i2c-handover` node on each side (`i2c_handover_he.overlay`, `hp_vision/boards/*.overlay`;
SDK glue in `zephyr/soc-bridge/alif/i2c_handover.c`, protocol in `i2c_handover.h`). The HE keeps
`&i2c1` disabled unless its shield needs the bus (the RVT121's fit overlay turns it back on), and
a disabled bus is released at once, so on the RK055 the HE never touches I2C1. Otherwise, at the end
of its boot the HE masks its I2C1 interrupt, stops its controller and publishes a per-boot nonce and
a state (`TR_MEM_I2C1_HANDOVER`, `0x0237FC94`, three words in SRAM0); a controller that will not go
idle is released DIRTY with a warning. The HP waits for a release it has not taken yet before it
unsticks the pads or initialises the bus and the camera (its unstick runs after the wait),
reports on its console every 20 s while the release is missing, and consumes it. Start the HE
first, then the HP. An HP restarted alone after it took the bus waits for a release that is not
coming, and an HE-only reset while the HP is running reconfigures the bus under it: restart both.
The touch controller is disabled in the fit overlay (the game does not use it). `TR_CAM_ROTATE` is
the camera's mounting rotation and is independent of the panel's; the camera view and skeleton are
drawn in the video area (the bottom 2/5) and rotated with the rest of the frame.

Limits: the 2D `TR_RENDER=M55` path cannot rotate and refuses a display with a `mount-rotation` at
build time. `TR_CAMERA` (a camera on the HE) is removed and refused at configure time: it had no keypoints, so no lane or jump. The sound image is unaffected (it uses I2C
bus 0 and I2S3, not I2C1). Build-only for the RVT121 shield flow until it is run on the bench.

## Sound (reworked carriers only)

`sound/` is an M55-HP image that plays the game's music and effects through the EVK's two
TAS2563 amplifiers on I2S3. **It refuses to build unless you pass `-DTR_SND_REWORKED_U46=ON`,
and only a carrier whose U46 has been reworked may run it.** On a stock 2626-R2 carrier, U46 is
a 74LVC157 with no high-impedance state, and it contends with the SoC the moment I2S3 is muxed
onto P9_3/P9_4/P9_5. `a32/release/sound-carriers.txt` is the per-unit allow list the release
script checks. Sound also needs a Zephyr tree that sets the 76.8 MHz audio clock; see
`docs/2026-09-23-sound.md`.

### Camera, NPU and sound in one HP image (`TR_HP_SOUND`)

The HP core has one image slot, so the exhibition release used to choose between `hp_vision`
(camera + NPU pose, no sound) and `sound/` (sound, no pose). Build `hp_vision` with
`-DTR_SND_REWORKED_U46=ON -DTR_HP_SOUND=ON` and it links the same game sound as a cooperative
thread above the vision thread (`TR_SND_EMBED`: I2S3 interrupt priority 0, ahead of the camera,
CSI and Ethos-U55, asserted at build time). The HE is built with the same `-DTR_HP_SOUND=ON`
(it needs `-DTR_INPUT_NPU=ON` and the Riverdi shield `-DSHIELD=e1m_evk_rvt121hvdfwca0`; the
RK055 shield puts its backlight on GPIO5, which the HP's amps need, and the build refuses it).

```sh
west build -b alp_e1m_aen803_m55_hp/ae822fa0e5597ls0/rtss_hp hp_vision -- \
  -DTR_CAM_ROTATE=90 -DTR_CAM_MIRROR=ON -DTR_SND_REWORKED_U46=ON -DTR_HP_SOUND=ON
west build -b alp_e1m_aen803_m55_he/ae822fa0e5597ls0/rtss_he . -- -DSHIELD=e1m_evk_rvt121hvdfwca0 \
  -DTR_INPUT_NPU=ON -DTR_HP_SOUND=ON -DTR_M55_AUTOLAUNCH=ON -DTR_A32_LAUNCH_H=<tr_launch.h>
```

The release is `TR_HP_VISION=ON TR_SND_HP=ON TR_SND_CARRIER_SERIAL=<unit>
a32/release/build-release.sh ...` and checks both `hp_vision_check.sh` and `snd_hp_check.sh` on that
one image (`a32/release/FLASH-RECIPE.md`, "Combined HP image"). Without `TR_HP_SOUND` the image
is the unchanged vision-only `hp_vision`.

**I2C2 and GPIO5 are shared, so the two cores take turns.** The HE uses I2C2 for the +5V INA236
(the HUD power line) and the BMI323; the HP's amp bring-up needs the same controller and GPIO5 (the
amps' `SD_N` and `IRQZ`). Two drivers on one DesignWare controller corrupt each other (its IRQ
reaches both cores), so the bus has one holder: after its display is up the HE *leases* I2C2 to the
HP once the HP asks, the HP brings the amps up (SD_N reset, ACK poll, `tas2563_init`, I2S3 proof,
`tas2563_resume`), turns its I2C2 interrupt off and *gives the bus back*. Streaming is I2S3 only. While the HP holds the
bus the HUD shows `5V -- mW` instead of a stale number. The protocol is `src/ipc/tr_bus2.h` (host
tested with both cores simulated: `tests/host/test_bus2.c`), its record sits at
`TR_MEM_BUS2 = 0x0237FD40`, clear of the I2C1 handover flag at `0x0237FC94`. It is not
`alp,i2c-handover`: that glue is one-way and makes the acquiring core wait at `POST_KERNEL` 0 until
the other core releases, which is right for the camera bus (no camera without it) and wrong here,
where the vision must never wait for the sound handshake and the HE needs the bus back. Fail safe:
no HP asking means the HE keeps the bus; no HE (or an HE built without `TR_HP_SOUND`) means the HP
waits and the vision runs without sound; an HP that never returns the bus leaves the HUD at `--`,
never a hang (and an HP that dies holding the lease, no beat for 2 s, is reclaimed by the HE:
controller stopped, SCL bus-clear); an HE restart in the middle of the bring-up aborts it before
the next access. The HP leases the bus only around the stretches that use it and keeps its I2C2
interrupt off in between, so an HE-only reset never meets an armed HP interrupt.

### Volume (`TR_HP_SOUND`)

The sound has a volume, 0 to 100 % of the build's `TR_SND_VOLUME` ceiling (128, the level heard as safe
on the reworked EVK's speakers; 100 % is that level and the default, so it can only be turned down). The
HP applies it as a software gain on every 16 ms block of mixed samples (`tr_vol_apply`, a linear ramp
across the block, no click, no I2C: I2C2 is leased) and 0 is silence with the amps still running (SD_N is
never toggled). The HE owns one word (`TR_MEM_VOL`, `0x0237FD80`, `src/ipc/tr_vol.h`, tagged `'VO'`: a cold
SRAM0 reads as 100 %) and sets it from:

- **The EVK's rotary encoder** (`BOARD_ENC_ROTARY`, read with `alp_qenc_*`): 5 % per detent, clockwise
  louder, clamped 0..100.
- **Its push switch** (`BOARD_PIN_ENCODER_SW`, `alp_gpio_*`): mute, press again for the last level.
- **A request word over SWD** (`0x0237FD84`, `0x564F0000 | percent`), for the bench: no reflash, see
  `a32/release/FLASH-RECIPE.md` ("Changing the volume without reflashing"). The HE validates it.

The HUD shows `VOL 40%` (or `MUTE`) over its bottom row for 1.5 s whenever the level changes. The encoder
pads are GPIO3 / GPIO4 (P3_0, P3_1, P4_3): not the GPIO5 port or the lpgpio island the HP's amps use. The
controls are not bench-verified on this image (the decode path is #2037 / #2095); `tests/host/test_vol.c`
covers the word, the gain and the HE's rules, `tests/host/test_hud.c` the popup.

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
