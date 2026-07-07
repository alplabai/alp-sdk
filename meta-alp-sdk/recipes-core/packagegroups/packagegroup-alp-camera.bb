# SPDX-License-Identifier: Apache-2.0
#
# Camera capture + media pipeline: libcamera (V4L2 capture) plus the
# GStreamer base stack apps build their pipelines on. OPT-IN via
# IMAGE_FEATURES += "alp-camera" (FEATURE_PACKAGES_alp-camera, set in
# alp-image-common.inc) so a headless / non-vision appliance carries none
# of it.
#
# Copyright (C) 2026 Alp Lab AB

SUMMARY = "Camera capture + media pipeline (libcamera + GStreamer)"

inherit packagegroup

RDEPENDS:${PN} = " \
    libcamera \
    gstreamer1.0 \
    gstreamer1.0-plugins-base \
"
