# V2N / V2N-M1 multimedia (opt-in)

`packagegroup-alp-multimedia` adds the GStreamer codec plugins, ffmpeg, the
Renesas hardware-codec (OMX) stack and operator tooling to a Linux image. It is
off by default: no shipped image (`alp-image-base`, `-prod`, `-edge`) enables it,
so `alp-image-prod` carries no commercial-flagged package unless you ask.

## Enable

In your own image recipe (or `local.conf` for a dev build):

```
IMAGE_FEATURES += "alp-multimedia"
```

Contents: `gstreamer1.0-plugins-good`, `-bad`, `gstreamer1.0-libav`,
`gstreamer1.0-rtsp-server`, `ffmpeg`, `nano`, `vim`, `v4l-utils`, `i2c-tools`.
On V2N-family MACHINEs (`MACHINE_FEATURES` has `v2n`) with `meta-rz-codecs` in
`bblayers.conf`, it also adds `codec-user-module`, `kernel-module-uvcs-drv` and
`gstreamer1.0-omx`, plus the ISU scaler/converter stack
(`gstreamer1.0-plugin-vspmfilter`, `vspmif-user-module`, `kernel-module-vspm`,
`kernel-module-vspmif`, `kernel-module-mmngr`, `mmngr-user-module`,
`mmngrbuf-user-module`; see "Camera to encoder"). Other MACHINEs never get the Renesas three (same gate as
`ALP_RZ_CODECS_RDEPENDS` in `packagegroup-alp-camera.bb`).

ffmpeg is built LGPL-only (`recipes-multimedia/ffmpeg/ffmpeg_%.bbappend` removes
`gpl`, `x264` and `postproc`, and sets the `ffmpeg` CLI package to
LGPL-2.1-or-later). A bbappend applies layer-wide, so every ffmpeg built with
meta-alp-sdk is LGPL-only, not just this package group. There is no hardware encode through ffmpeg.

## Licence flags you must accept

These recipes are `LICENSE_FLAGS = "commercial"`; without the lines below the
build stops with an "incompatible license" error. Accepting them is a legal
decision for your product (patent-encumbered codecs), not a technical one.

```
LICENSE_FLAGS_ACCEPTED += "commercial_ffmpeg commercial_gstreamer1.0-libav commercial_gstreamer1.0-omx"
```

## Open versus closed

| Part | Licence | Note |
|---|---|---|
| `codec-user-module` (VCD user libraries, 4.3.4.0) | CLOSED | Renesas package, login-gated download; cannot ship in public alp-sdk |
| `mali-library` (GPU DDK) | CLOSED | Also the source of OpenCL |
| `kernel-module-uvcs-drv` | GPL-2.0-only and MIT | Open, but useless without the closed user module |
| `gstreamer1.0-omx` | LGPL wrapper, `commercial` flag | Open wrapper over the closed OMX libraries |
| ffmpeg, gstreamer1.0-libav | LGPL here, `commercial` flag | Software codecs |

There is no openly licensed hardware H.264 path on V2N: the encoder is reachable
only through the closed Renesas libraries.

## Graphics compute

- **Vulkan is not available.** `meta-rz-graphics` removes the `vulkan`
  DISTRO_FEATURE (`conf/layer.conf:17-19`) and the Mali DDK ships no Vulkan ICD.
  Use GLES or OpenCL.
- **OpenCL comes from `libmali`**, which installs its own `libOpenCL.so`. Do not
  also install the Khronos `opencl-icd-loader` without testing for a provider
  collision.

## Camera to encoder (hardware path)

The encoder (`omxh264enc`, `omxh265enc`) takes only NV12 or I420. The V2N CRU
delivers packed CR10 (mono) or RAW10 Bayer. `vspmfilter` is the Renesas
GStreamer element that drives the ISU (image scaling unit) through VSPM; it is
in this package group on V2N machines.

### What vspmfilter is, from source and the Renesas manual

Evidence: `renesas-rz/rzg_gstreamer_vspmfilter` branch `rz_g2l` at the recipe's
pinned SRCREV `221c5349`; the recipe is
`meta-rz-distro/recipes-multimedia/gstreamer/gstreamer1.0-plugin-vspmfilter.bb`;
"RZ/V2H and RZ/V2N Linux Interface Specification GStreamer" R01US0653EJ0203
Rev.2.03 section 3.3.6 and Fig 2-5.

- Drives the ISU (`VSPM_ISU_PAR`, `ISU_*` formats in `gstvspmfilter.c`), not VSPI/VSPD.
- Sink and src caps: NV12, RGB16, RGB, BGR, RGBx, BGRx, xRGB, xBGR, RGBA, BGRA,
  ARGB, ABGR, UYVY, YUY2, NV16, GRAY8, GRAY10_LE64 (the last only if the
  GStreamer base headers define it). Width and height 1..4096.
- No Bayer caps. GRAY8 and GRAY10_LE64 map to `ISU_RAW8`/`ISU_RAW10`, a single
  component plane. RAW to YUV uses the matrix `csc_k_matrix_e` (Y = raw, chroma
  offset 0x80), so the result is **grey NV12, not a demosaic**.
- Downscale only. `x_ratio = (in_w << 12) / out_w` must be at least 1.0 or the
  element errors "does not support scale up"; the ratio field is 16 bits, so
  under 16x. There is no crop property (crop is fixed to the output size).
- The kernel side (`kernel-module-vspm`, patch 0017 for the rzv2n family) allows
  4096x4096 input; the pre-0017 limit was 2800x2047.
- Buffers must be physically contiguous: it finds the physical address with
  `MM_IOC_VTOP` (mmngr memory) or imports a dma-buf (`mmngr_import_start_in_user_ext`).
  A frame whose address cannot be resolved is **silently skipped**
  (`gstvspmfilter.c`, "W/A" branch). Properties: `dmabuf-use`, `outbuf-alloc`.

### Is it in the images today?

No. `vspmfilter` is absent from the build manifests of `alp-image-edge` for
`e1m-v2n101-a55` (build-372) and `e1m-v2m103-a55` (build-isp568). Only
`kernel-module-vspm`, `kernel-module-vspmif`, `kernel-module-mmngr`,
`libvspm1` and `libmmngr1` are present, pulled in by the codec stack. It is
installed only through `packagegroup-gstreamer1.0-plugins` (Renesas' own image
set), which no Alp image uses. Enable `alp-multimedia` to get it.

### The capture side is the gap

The Renesas `gst-plugins-good` fork (`RZ/1.22.12`, SRCREV `93c06ee`) has no
mmngr or CRU-specific code in `sys/v4l2/`. Its `v4l2src` maps GREY to GRAY8 and
only one Bayer fourcc (`SRGGB8`, 8-bit RGGB). `Y10`, `Y10BPACK` and the CRU
10-bit packed fourcc fall into "Unsupported fourcc"; there is no GRAY10_LE64
mapping. The manual's `v4l2src` table (3.3.5) lists BGR, GRAY8, YUY2, UYVY,
NV16, ARGB, BGRA, BGRx at 80..1920 x 80..1080, up to 60 fps. So `v4l2src` cannot
hand CR10 to `vspmfilter`, and the 1920x1080 cap is in the manual. An element
that turns CR10 into GRAY8 or GRAY10_LE64 must sit before `vspmfilter`:
`alpcamerasrc` (issue #3117) is that element. For zero copy it would have to
allocate mmngr/dma-buf memory; the system-memory buffers it uses today are not
resolvable by `vspmfilter` (see above).

The Renesas `gst-plugins-bad` fork adds `bayerconvert`: OpenGL ES Bayer to
BGRx, 8-bit `video/x-bayer,format=rggb` only, linked against `libbayer2raw`.
No layer in the tree provides `libbayer2raw`, so it is not usable from this
layer set.

### Pipelines

Verified = stated by the manual or read from source. Nothing below has run on
hardware (no camera available when this was written).

(a) Mono (IMX568 or OV9281) to H.264 1080p:
```
alpcamerasrc ! video/x-raw,format=GRAY8,width=2464,height=2064 ! \
  vspmfilter dmabuf-use=TRUE ! video/x-raw,format=NV12,width=1288,height=1080 ! \
  omxh264enc ! h264parse ! ...
```
H.264 is capped at 1920x1080 by the silicon, so full-frame IMX568 (2464x2064)
must be downscaled; 1288x1080 keeps the aspect ratio. OV9281 (1280x800) needs no
scaling: `alpcamerasrc ! video/x-raw,format=NV12 ! omxh264enc`. vspmfilter
replaces alpcamerasrc's CPU NV12 fill and adds the downscale in one ISU pass.
Unverified: that alpcamerasrc buffers are ISU-addressable, and ISU on RAW8.

(b) Colour Bayer (IMX296/IMX568) without ISP: the ISU does not demosaic. Use
`alpcamerasrc` (2x2 superpixel, half-size NV12: IMX568 gives 1232x1032, already
inside the H.264 limit, so no scaling is needed; IMX296 gives 728x544). Add
`vspmfilter` only to scale further or to convert to RGB for display. Sequence:
`alpcamerasrc ! video/x-raw,format=NV12 ! [vspmfilter ! video/x-raw,format=NV12,width=W,height=H !] omxh264enc`.
`bayerconvert` is the only Renesas demosaic element and is not buildable here.

(c) With the Mali-C55 ISP: its output is NV12 or RGB already. For H.264 NV12
at 1920x1080 or less it feeds `omxh264enc` directly; use `vspmfilter` only to
downscale, or to convert RGB to NV12. The manual's capture example (4.4.1) is
`v4l2src ! video/x-raw,format=UYVY,width=1920,height=1080 ! vspmfilter dmabuf-use=TRUE ! video/x-raw,format=NV12 ! omxh264enc control-rate=2 target-bitrate=10485760 use-dmabuf=TRUE ! h264parse`.

### Bench checks on E1M-V2M103

Load first: `lsmod | grep -E 'vspm|mmngr'`, `ls /dev/vspm* /dev/mmngr*`,
`gst-inspect-1.0 vspmfilter` (caps must list GRAY8 and NV12).

ISU test with no camera (videotestsrc, GRAY8 downscale to NV12):
```
gst-launch-1.0 -v videotestsrc num-buffers=60 ! video/x-raw,format=GRAY8,width=2464,height=2064 ! \
  vspmfilter dmabuf-use=TRUE ! video/x-raw,format=NV12,width=1288,height=1080 ! \
  omxh264enc ! h264parse ! mp4mux ! filesink location=/tmp/isu.mp4
```
Pass: exit 0, `/tmp/isu.mp4` plays at 1288x1080. If the file is empty and no
error appears, the frame-skip branch fired (buffer not physically resolvable);
retry with `videotestsrc ! ... ! vspmfilter` fed by a dma-buf source.
Scale-up check: width 640 to 1280 must fail with "does not support scale up".

Camera pipelines: run (a) and (b) above with `alpcamerasrc` on the camera and
check `dmesg | grep -i vspm` for ISU errors, `top` for CPU load against a
CPU-only NV12 run, and the encoded file. For (c) use the ISP's V4L2 node.
