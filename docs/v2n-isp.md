# RZ/V2N ISP (Arm Mali-C55): Bayer sensors through `<alp/camera.h>` (Linux, opt-in)

**Status: BENCH-UNVERIFIED.** Nothing here has run on a board. The backend
change is unit-tested against a scripted ioctl hook; the Yocto and
device-tree pieces were written by inspection and have not been through
`bitbake` or `dtc` against the vendor tree. No sensor has streamed through
the ISP.

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
| ISP half of the CAM0 dtb, boot unit `alp-isp-init`, patch reconciliation | `meta-alp-sdk/dynamic-layers/meta-rz-isp/` | integration, no driver code |

No ISP driver, sensor tuning or calibration is written here, and the portable
`<alp/camera.h>` surface gained no vendor symbol.

## Licence and where the package lives

The package (Renesas part number RTK0EC0004S01004SJ, v1.31) is
licence-gated: its documents are marked confidential and the userspace ISP
daemon ships as a prebuilt binary under a closed licence (the kernel patches
are GPL-2.0). **None of it is in this repository.** Alp-built images take the
package from the private mirror (`alp-sdk-internal`,
`vendors/renesas-rzv2n/isp-support-package-v1.31/`); everyone else obtains it
from Renesas under their own terms. This repo only carries code that is
parsed when that layer is present (`meta-alp-sdk/dynamic-layers/meta-rz-isp/`)
and the layer-agnostic backend.

## Sensor support: read this before enabling

The package ships ISP support for a single reference sensor (the Sony
IMX415) and nothing else is supported out of the box. **IMX219 does not work
with the package as shipped.** Streaming a different sensor through the ISP
needs an ISP sensor driver, calibration data for that sensor, and an input
path matching its bit depth and lane count. How to do that is described in
the Renesas RZ/V2N ISP Support Package documentation (R01AN8271, R01US0778),
which you obtain from Renesas under their terms.

That port is not in this change (open question in the PR description). The
tree is otherwise ready for it: the device-tree fragment, patch handling,
boot unit, backend and HIL spec are sensor-agnostic.

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
   (`ALP_ENABLE_CAM0_*`; OV9281 is refused on an ISP image). The ISP half is only added on top
   of a selected CAM0 sensor.
3. Build: `ALP_ENABLE_ISP` defaults to `1` when the layer is present (`0` opts
   out of our half only).

   ```
   MACHINE=e1m-v2m103-a55 bitbake alp-image-edge     # also e1m-v2n101-a55
   ```

The dtb to boot is `renesas/e1m-v2{n,m}101-x-evk-cam0-isp.dtb`; the
bootloader `fdtfile` must name it (the plain cam0 dtb stays as the fallback
but has no ISP nodes).

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
- **Kernel patches.** The package rewrites the CRU/CSI-2 driver. Our patches
  `0016` (CRU Y10/Y8 greyscale) and `0017` (CSI-2 lane polarity via SWAPCTL)
  edit the same files and cannot apply on top, so the layer drops them and
  warns at parse time. Consequences on an ISP image: no mono sensors (OV9281),
  and no lane-polarity swap, which E1M-V2M103 + X-EVK J5 needs. Porting `0017`
  onto the package's `rzg2l-csi2.c` (it already defines the SWAPCTL register)
  is **open**.
- **First build.** Expect the first failures, if any, in `do_patch` of the
  package's kernel series (it and our `0001` clock patch touch the same
  source file; the hunks are expected to apply with an offset but this has
  not been run), then `dtc`, then `alp-isp-init`.
- **Machine compatibility.** The package restricts its kernel append and
  its two userspace recipes to its own EVK machine; this layer widens that
  match to the Alp V2N/V2M machines (`v2n-isp-*_%.bbappend`).
- **Boot.** `alp-isp-init.service` runs the package's `v4l2-init.sh` (installed in root's home directory)
  (media pipeline, ISP sensor preset, crop, ISP userspace daemon). Mode
  defaults to `2k` (1920x1080); set `ISP_MODE` in `/etc/default/alp-isp`.
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

- IMX219 (see above), dual sensors, HDR/DOL presets from the API, suspend/resume.
- The Zephyr/CM33 camera backend (`v2n_n44_isp.c`) is unrelated and untouched.
- Raw capture through the CRU on an ISP image (the ISP owns the CRU).

## Verification

Unit: `tests/yocto/peripheral_camera_isp.c`. Bench:
[`tests/hil/v2m103-x-evk/v2m103-isp-capture.yaml`](../tests/hil/v2m103-x-evk/v2m103-isp-capture.yaml)
(IMX219 on CAM0; expected to fail until the sensor port exists). Report
results on the tracking issue before treating any of this as verified.
