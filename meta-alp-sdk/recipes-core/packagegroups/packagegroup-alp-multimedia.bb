# SPDX-License-Identifier: Apache-2.0
#
# Multimedia + operator tooling: GStreamer good/bad/libav/rtsp-server, LGPL-only
# ffmpeg, nano, vim, v4l-utils, i2c-tools, and on V2N-family machines the
# Renesas OMX hardware-codec stack. OPT-IN via IMAGE_FEATURES += "alp-multimedia"
# (FEATURE_PACKAGES_alp-multimedia, alp-image-common.inc); no shipped image
# enables it, so alp-image-prod carries no commercial-flagged package unless
# the integrator turns it on. Several members carry LICENSE_FLAGS = "commercial"
# and the build fails until they are accepted: see docs/v2n-multimedia.md.
#
# Copyright (C) 2026 Alp Lab AB

SUMMARY = "Multimedia (GStreamer, ffmpeg, Renesas OMX) and operator tooling"

# RDEPENDS varies by MACHINE_FEATURES, so MACHINE_ARCH before the class
# defaults to allarch -- same reason as packagegroup-alp-camera.bb.
PACKAGE_ARCH = "${MACHINE_ARCH}"

inherit packagegroup

RDEPENDS:${PN} = " \
    gstreamer1.0-plugins-good \
    gstreamer1.0-plugins-bad  \
    gstreamer1.0-libav        \
    gstreamer1.0-rtsp-server  \
    ffmpeg                    \
    nano                      \
    vim                       \
    v4l-utils                 \
    i2c-tools                 \
"

# Renesas VCD stack (codec-user-module CLOSED, kernel-module-uvcs-drv, the
# gst-omx wrapper). Same two gates as packagegroup-alp-camera.bb's
# ALP_RZ_CODECS_RDEPENDS: meta-rz-codecs present AND MACHINE_FEATURES "v2n",
# so a non-RZ MACHINE sharing the bblayers.conf never gets a dependency with
# no provider.
ALP_RZ_OMX_RDEPENDS = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', \
    bb.utils.contains('MACHINE_FEATURES', 'v2n', 'codec-user-module kernel-module-uvcs-drv gstreamer1.0-omx', '', d), '', d)}"
ALP_RZ_OMX_RDEPENDS[vardepvalue] = "${ALP_RZ_OMX_RDEPENDS}"

RDEPENDS:${PN} += "${ALP_RZ_OMX_RDEPENDS}"

# Renesas ISU (image scaling unit, driven through VSPM) GStreamer filter: scales
# and converts GRAY8/NV12/RGB to NV12 in hardware. Same two gates as the OMX
# stack above, because the RZ/V2N ISU kernel patches live in meta-rz-codecs
# (kernel-module-vspm.bbappend). Listed explicitly even though vspmfilter
# RDEPENDS most of them, so the rpm set is visible here.
ALP_RZ_ISU_RDEPENDS = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', \
    bb.utils.contains('MACHINE_FEATURES', 'v2n', 'gstreamer1.0-plugin-vspmfilter vspmif-user-module kernel-module-vspm kernel-module-vspmif kernel-module-mmngr mmngr-user-module mmngrbuf-user-module', '', d), '', d)}"
ALP_RZ_ISU_RDEPENDS[vardepvalue] = "${ALP_RZ_ISU_RDEPENDS}"

RDEPENDS:${PN} += "${ALP_RZ_ISU_RDEPENDS}"
