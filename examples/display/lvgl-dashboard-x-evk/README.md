# lvgl-dashboard-x-evk

A minimal LVGL 9 dashboard on the E1M-X V2N MIPI-DSI panel
(RK055HDMIPI4MA0, 720 x 1280 @ 60 Hz).

## What it shows

- On the Renesas RZ/V2N family, the display pipeline is plain Linux
  DRM/KMS (`rz-du` driver).  There is no `alp_display_*` indirection
  on Linux -- apps talk to the DRM stack directly, or through a toolkit
  such as LVGL.
- LVGL 9's Linux DRM backend (`lv_linux_drm`) scans out fullscreen on
  the panel using double-buffered dumb BOs; no GPU or Wayland required.
- Touch arrives as a standard evdev pointer once the GT911 controller
  is reachable from Linux (see note below).
- The example is intentionally board-specific (`som.sku: E1M-V2N101`);
  any V2x SKU works -- the PCB and panel path are the same across
  V2N101 / V2N102 / V2M101 / V2M102.

## Hardware needed

- E1M-X V2N or V2M SoM on the X-EVK carrier.
- LCD panel (RK055HDMIPI4MA0) connected to the carrier's Display 1
  connector (J6).  The panel must be powered -- check the backlight
  enable line (GPT1 CH2 on the SoM, driven by the carrier DT).
- For touch (optional): the GT911 controller is available once the
  GD32 I2C-proxy follow-up task adds a Linux master path to BRD_I2C.
  The dashboard runs display-only without it.

## Consumer path 1 -- Yocto image (recommended)

The `alp-lvgl-dashboard` Yocto recipe (in
`meta-alp-sdk/recipes-examples/alp-lvgl-dashboard/`) packages this
binary into `alp-image-edge`.

1. Build the image with the standard Yocto flow (see the SDK docs).
2. Flash and boot.
3. If Weston is running, stop it first so it releases the DRM master:

   ```
   systemctl stop weston
   ```

4. Run the dashboard:

   ```
   alp-lvgl-dashboard
   ```

   The panel should light up immediately showing the ALP SDK title,
   an arc gauge set to 62%, and a "Touch me" button.

## Consumer path 2 -- standalone cross-compile

Hand-written firmware that bypasses alp-studio or Yocto is
first-class.  Cross-compile against the SDK sysroot (which contains
the target's LVGL 9 shared library):

```
cmake \
  -DCMAKE_TOOLCHAIN_FILE=<sysroot>/toolchain.cmake \
  -S examples/display/lvgl-dashboard-x-evk \
  -B build/dash
cmake --build build/dash

# Copy to the board
scp build/dash/alp-lvgl-dashboard root@<board-ip>:

# On the board
systemctl stop weston
./alp-lvgl-dashboard
```

The `EXTRA_OECMAKE` line in the Yocto recipe passes
`-DALP_OS=yocto`; for a standalone build this variable is not
required -- CMake just needs `lvgl` in the library search path, which
the SDK sysroot provides.

## Touch note

On current V2N hardware, the GT911's I2C bus terminates at the GD32
IO-MCU (BRD_I2C), not directly at an RZ/V2N RIIC controller.  Until
the GD32 I2C-proxy follow-up adds a Linux-visible I2C adapter,
`/dev/input/event0` will not exist and the dashboard runs
display-only.  The `access(2)` guard in `main.c` handles this
gracefully -- no modification needed once touch becomes available.

## Verification status

`[UNTESTED on silicon]` -- CMake parses cleanly; LVGL 9.1 API verified
against the upstream v9.1.0 tag.  Real-hardware bring-up is gated on
the display pipeline patches landing in the Yocto layer (Tasks 1-7 of
the v2n-lcd-display1 branch).
