# SPDX-License-Identifier: Apache-2.0
#
# Development image for V2N / V2N-M1 / i.MX 93 edge AI bring-up.
# Built by:
#   MACHINE = "e1m-v2m101-a55" bitbake alp-image-edge
#
# = alp-image-common (the shared runtime: Alp SDK, ROS 2 + alp_perception,
#   GStreamer/libcamera, Mender, Weston, watchdog, networkd) PLUS developer
#   conveniences:
#     - debug-tweaks: empty root password / passwordless console + SSH login
#       (bench convenience -- the production image, alp-image-prod, strips it).
#     - libdrm-tests: modetest et al. for display bring-up.
# For a hardened, customer-facing build use alp-image-prod instead.

SUMMARY = "Alp SDK edge AI development image (debug-tweaks, bench tooling)"

require alp-image-common.inc

IMAGE_FEATURES += "debug-tweaks"

# Display/bench bring-up tooling (modetest, modeprint). Rides in rz-vlp's tools
# group too; listed explicitly so rz-bsp builds also get it. Dev-only.
IMAGE_INSTALL += " \
    libdrm-tests                   \
"

# LVGL dashboard example (Linux/DRM panel) -- dev-only bench example app.
# weston/libdrm/DEEPX/rootfs sizing now come from alp-image-common.inc;
# only the example package is edge-specific.
IMAGE_INSTALL += " alp-lvgl-dashboard"
