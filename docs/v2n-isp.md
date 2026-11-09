# RZ/V2N ISP (Arm Mali-C55): Bayer sensors through `<alp/camera.h>` (Linux, opt-in)

**Status: BENCH-UNVERIFIED.** Nothing here has run on a board. The backend
change is unit-tested against a scripted ioctl hook. The Yocto pieces were
built against ISP Support Package V1.50 (`do_patch`, kernel compile, the ISP
userspace recipes) and the device tree went through `dtc` as part of the
kernel build. No sensor has streamed through the ISP.

The RZ/V2N carries an Arm Mali-C55 ISP (Renesas name IV021). Without it a
Bayer sensor can only be read raw through the CRU
([`v2n-camera-csi.md`](v2n-camera-csi.md)): `<alp/camera.h>` returns
`ALP_PIXFMT_RAW8` / `RAW10` and no colour. With the Renesas ISP Support
Package on the image, the same `alp_camera_open()` returns demosaiced,
auto-exposed, auto-white-balanced `ALP_PIXFMT_RGB565` or `ALP_PIXFMT_NV12`
frames.

## What we consume and what we write (ADR 0017)

| Piece | Source | Tier |
|---|---|---|
| ISP + IVC kernel drivers, CRU/CSI-2 rework, sensor/lens/iq sub-drivers, SoC device-tree nodes | Renesas ISP Support Package (kernel patch series) | **Tier-1**: consumed as shipped, not forked or rewritten |
| ISP userspace control daemon (AE/AWB), `v4l2-init.sh` | Renesas ISP Support Package | **Tier-1**: consumed as shipped |
| `src/backends/camera/yocto_isp_capture.h` (read frames from the ISP's V4L2 node) | this repo | **Tier-1.5**: thin glue over a vendor V4L2 node; no ISP register is touched |
| ISP half of the CAM0 dtb, boot unit `alp-isp-init`, patch reconciliation | `meta-alp-sdk/dynamic-layers/meta-rz-isp/` | integration |
| CSI-2 lane-polarity patch for the package's CSI-2 receiver driver (`isp-0001`) | `meta-alp-sdk/dynamic-layers/meta-rz-isp/` | our GPL-2.0 kernel change, a port of patch `0017` |
| ISP sensor sub-driver, IQ entry points and init-script edits for the Sony IMX568C | private mirror (`alp-sdk-internal`), overlay layer `meta-alp-isp-imx568` | derived from the package, not public |

No ISP driver, sensor tuning or calibration is in this repository, and the
portable `<alp/camera.h>` surface gained no vendor symbol.

## Licence and where the package lives

The package (Renesas part number RTK0EC0005S01001SJ, version V1.50) is
licence-gated: its documents are marked confidential and the userspace ISP
daemon ships as a prebuilt binary under a closed licence (the kernel patches
are GPL-2.0). **None of it is in this repository.** Alp-built images take the
package from the private mirror (`alp-sdk-internal`,
`vendors/renesas-rzv2n/isp-support-package-v1.50/`); everyone else obtains it
from Renesas under their own terms. This repo only carries code that is
parsed when that layer is present (`meta-alp-sdk/dynamic-layers/meta-rz-isp/`)
and the layer-agnostic backend.

## Sensor support: read this before enabling

The package ships an ISP sensor sub-driver for a single reference sensor (the
Sony IMX415) and nothing else is supported out of the box: other sensors
(**IMX219, IMX335, ...**) do not stream through the ISP. A new sensor needs
an ISP sensor sub-driver, calibration (IQ) data for that sensor and module,
and an input path matching its bit depth; the package's sensor replacement
guide describes it and is obtained from Renesas under their terms.

One port exists: the **Vision Components MIPI IMX568C** colour global-shutter
module (`ALP_CAMERA_CAM0 = "vc_mipi_imx568c"`, [`v2n-camera-csi.md`](v2n-camera-csi.md)).
Its sub-driver, IQ entry points and init-script edit are derived from the
package and live in the private mirror as the overlay layer
`meta-alp-isp-imx568`; without that layer the build warns and the ISP still
drives the IMX415 sub-driver. Scope of the port:

- 2464x2064 RAW12, Bayer RGGB, 2 or 4 lanes (the lane count follows the DT).
  RAW10 is not implemented: the ISP input bit width is fixed at build time.
- The module's own V4L2 driver (`vc_mipi`) keeps power, mode registers and
  streaming; the ISP side only forwards exposure and gain to its V4L2
  controls.
- **The IQ data is UNTUNED**: the generic tables shipped with the package with
  black level and white-balance reference adjusted. Colour accuracy, AWB
  convergence, lens shading and noise reduction are those of the reference
  sensor until the module is calibrated with the Arm Mali-C55 calibration tool
  that ships with the package (a bench task, needs charts and illuminants).
- Mono sensors (OV9281, `vc_mipi_imx568`) are refused on an ISP image.

## Enable it (image build)

Build host prerequisites are the same as [`build-yocto-v2n.md`](build-yocto-v2n.md)
(RZ/V2N AI SDK v6.30 layers).

1. Stage the package layer and put it on the layer list. Alp builds use the
   mirror's helper:

   ```
   ISP_LAYER_DIR=<dir> ./stage-isp-layer.sh     # from the private mirror
   bitbake-layers add-layer "$ISP_LAYER_DIR/meta-rz-features/meta-rz-isp"
   ```

   Do not add `meta-econsys` in the same build.
2. Pick a CAM0 sensor as in [`v2n-camera-csi.md`](v2n-camera-csi.md)
   (`ALP_CAMERA_CAM0 = "<module_id>"`; mono modules are refused on an ISP
   image). The ISP half is only added on top of a selected CAM0 module.
3. Build: `ALP_ENABLE_ISP` defaults to `1` when the layer is present (`0` opts
   out of our half only).

   ```
   MACHINE=e1m-v2m103-a55 bitbake alp-image-edge     # also e1m-v2n101-a55
   ```

   For the IMX568C also add the overlay layer from the same mirror directory
   (`bitbake-layers add-layer <mirror>/meta-alp-isp-imx568`, after the package
   layer) and set `ALP_CAMERA_CAM0 = "vc_mipi_imx568c"`.

The ISP nodes are folded into the shipped `renesas/e1m-v2{n,m}101-x-evk.dtb` (like the CAM0
fragment: U-Boot resets `fdtfile` to that name on every boot), so no
bootloader change is needed. They are added only for a CAM0 module that has an
ISP sensor sub-driver (today the IMX568C); any other colour module needs
`ALP_ENABLE_ISP = "0"` and the build stops with that advice otherwise.

### What our layer does when the package is present

- **Device tree.** `e1m-x-evk-cam0-isp.dtsi` turns on the package's
  `&input_video_control`, `&isp`, `&sensor`, `&lens`, `&iq` nodes (created by
  the package's SoC dtsi patch, which owns their registers, clocks and
  interrupts) and points `sensor-i2c` at the generated CAM0 sensor
  (`cam0_sensor`). The sensor, CSI-2 and CRU graph stays in the camera DT
  generator's fragment: nothing is stated twice.
  `e1m-v2n-som.dtsi` labels the reserved ISP buffer `isp_reserved`, the name
  the package uses, so the package's `isp:` label is not a duplicate. The DDR
  map is unchanged.
- **Kernel patches.** The package rewrites the CRU/CSI-2 driver. Our patch
  `0016` (CRU Y10/Y8 greyscale) edits the same files and cannot apply on top,
  so the layer drops it: no mono sensors on an ISP image. Our patch `0017`
  (CSI-2 lane polarity via SWAPCTL, which E1M-V2M103 + X-EVK J5 needs) is
  dropped too and replaced by `isp-0001`, the same change written against the
  package's CSI-2 driver (`lane-polarities` on the receiver endpoint programs
  `CRUm_SWAPCTL` bits 5/4; mixed data-lane polarities are refused). It is added
  from an anonymous python function so it applies after the package series.
- **Package series.** The package adds its RZ/V2N kernel patches (`0001-...-v2n`,
  `0101`..`0104`) under the machine override `rzv2n-evk`, which no Alp machine
  carries. The layer splices them into `SRC_URI` at the package's own positions
  (and stops the build if the series is not the V1.50 layout), then appends
  `isp-0001`.
- **Build status.** With V1.50 and the IMX568C overlay, `e1m-v2m103-a55`:
  `do_patch` applies all 53 patches in order, the kernel (ISP built in) and the
  shipped `e1m-v2m101-x-evk.dtb` (with the ISP nodes) build, as do `rzv-isp-scripts`, `rzv-isp-driver`
  and `alp-isp-init`. Not booted.
- **Machine compatibility.** The package restricts its kernel append and
  its two userspace recipes to its own EVK machines; this layer widens that
  match to the Alp V2N/V2M machines (`rzv-isp-*_%.bbappend`).
- **Boot.** `alp-isp-init.service` runs the package's `v4l2-init.sh` (installed in root's home directory)
  (media pipeline, ISP sensor preset, crop, ISP userspace daemon). Mode
  defaults to `2k` (IMX415, 1920x1080) or `full` (IMX568C overlay, 2464x2064);
  set `ISP_MODE` in `/etc/default/alp-isp`.
  The image gains it only when the layer, `ALP_ENABLE_ISP=1` and the `v2n`
  machine feature all hold.

## Application API

No new API. Open camera N with an ISP-produced format:

```c
alp_camera_config_t cfg = ALP_CAMERA_CONFIG_DEFAULT(0);  /* RGB565 */
cfg.width = 1920; cfg.height = 1080;
alp_camera_t *cam = alp_camera_open(&cfg);
```

`yocto_drv` tries the ISP first: `/dev/video<N>fr` present, a multi-planar
capture node, and the format is `ALP_PIXFMT_RGB565` or `ALP_PIXFMT_NV12`. The
node must accept the requested size as-is (readback checked). Anything else
(no ISP, `GREY8` / `RAW8` / `RAW10`) takes the raw media-controller path
unchanged; colour on an image without the package stays `ALP_ERR_NOSUPPORT`.
`alp_camera_configure_isp()` stays `ALP_ERR_NOSUPPORT` on this path: AE/AWB
belong to the ISP daemon, whose controls are not mapped onto
`alp_camera_isp_config_t`. The first frame can take seconds after boot while
`alp-isp-init` runs; a timeout names it.

## Not covered

- IMX219 and every sensor but the IMX415 and the IMX568C (see above), dual
  sensors, HDR/DOL presets from the API, suspend/resume.
- The Zephyr/CM33 camera backend (`v2n_n44_isp.c`) is unrelated and untouched.
- Raw capture through the CRU on an ISP image (the ISP owns the CRU).

## First bring-up of an IMX568C module

No hardware has run this. Order matters: prove each layer before the next.

1. Build the ISP image as above (V1.50 package + overlay), boot the shipped
   `e1m-v2m101-x-evk.dtb` (it carries the CAM0 and ISP nodes).
2. Module probe: `dmesg | grep -i vc-mipi` shows the module descriptor (sensor
   IMX568, colour, lanes). `media-ctl -p -d /dev/media0` lists the sensor, the
   `rzg2l_csi2` receiver and the CRU.
3. Lane polarity: if no frames arrive, read the receiver status; the J5
   connector needs `lane-polarities = <1 1 1>` (generated into the CAM0
   fragment) and `isp-0001` makes it take effect.
4. For the first ISP boot use a debug kernel (`CONFIG_DEBUG_ATOMIC_SLEEP`,
   lockdep) and watch `dmesg` for sleeping-in-atomic or lockdep reports from the
   ISP sensor sub-driver.
5. ISP node: `/dev/video0fr` appears, `systemctl status alp-isp-init` is
   active, and `dmesg` has no "no V4L2 sensor sub-device behind ISP bus 1" line
   (a missing sub-device is logged for context 0 only).
6. Log the D-PHY rate the CRU actually uses (the ISP sensor sub-driver refreshes its
   `mbps` at mode set; check that the IVC copies it afterwards) before trusting a
   first stream.
7. Stream: `v4l2-ctl -d /dev/video0fr --stream-mmap --stream-count=60`, then
   an `alp_camera_open()` capture (`tests/hil/v2m103-x-evk/v2m103-isp-capture.yaml`).
8. Check that exposure and gain follow the scene (cover the lens, watch the
   module's `exposure` and `gain` controls), then calibrate (private
   procedure in the mirror).

## Verification

Unit: `tests/yocto/peripheral_camera_isp.c`. Bench:
[`tests/hil/v2m103-x-evk/v2m103-isp-capture.yaml`](../tests/hil/v2m103-x-evk/v2m103-isp-capture.yaml)
(IMX568C on CAM0; expected to fail until the first bring-up above has been
done). Report results on the tracking issue before treating any of this as
verified.
