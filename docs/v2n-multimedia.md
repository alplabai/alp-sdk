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
`gstreamer1.0-omx`. Other MACHINEs never get the Renesas three (same gate as
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
