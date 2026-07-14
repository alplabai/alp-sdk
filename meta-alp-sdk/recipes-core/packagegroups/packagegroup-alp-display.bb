# SPDX-License-Identifier: Apache-2.0
#
# Display stack (X-EVK MIPI-DSI panel): Weston on Mali/Wayland -- the
# wayland+opengl distro features come from rz-vlp. OPT-IN via
# IMAGE_FEATURES += "alp-display" (FEATURE_PACKAGES_alp-display, set in
# alp-image-common.inc) so a headless appliance ships no compositor.
# The dev-only modetest/libdrm-tests ride in alp-image-edge, not here.
#
# Copyright (C) 2026 Alp Lab AB

SUMMARY = "Wayland/Weston display stack (Mali/Wayland)"

inherit packagegroup

RDEPENDS:${PN} = " \
    weston \
    weston-init \
    libdrm \
"
