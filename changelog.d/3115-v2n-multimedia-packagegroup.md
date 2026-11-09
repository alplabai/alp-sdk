### Added — Opt-in V2N multimedia package group

**`IMAGE_FEATURES += "alp-multimedia"` installs `packagegroup-alp-multimedia`: GStreamer good/bad/libav/rtsp-server, an LGPL-only ffmpeg, nano, vim, v4l-utils, i2c-tools and, on V2N-family machines with `meta-rz-codecs`, the Renesas OMX codec stack.** No shipped image enables it, so `alp-image-prod` stays free of commercial-flagged packages; the `LICENSE_FLAGS_ACCEPTED` lines and the open-versus-closed split (including no Vulkan on the Renesas BSP) are in `docs/v2n-multimedia.md`.
