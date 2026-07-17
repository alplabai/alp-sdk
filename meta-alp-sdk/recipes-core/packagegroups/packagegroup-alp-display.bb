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

# No explicit libdrm RDEPENDS: `inherit packagegroup` makes this group
# allarch, and an allarch package must not RDEPEND on a package that gets
# arch-renamed (libdrm -> libdrm2 via debian.bbclass) -- BitBake flags that
# as an error at do_package_write_rpm. weston's kms/drm PACKAGECONFIG links
# libweston's drm-backend.so against libdrm.so.2, so the shlibs mechanism
# already adds the (arch-specific) libdrm2 runtime dependency automatically
# once weston is installed; no explicit line is needed here.
RDEPENDS:${PN} = " \
    weston \
    weston-init \
"
