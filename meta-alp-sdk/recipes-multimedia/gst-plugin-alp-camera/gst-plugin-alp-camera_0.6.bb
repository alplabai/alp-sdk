# SPDX-License-Identifier: Apache-2.0
#
# alpcamerasrc: GStreamer source built on <alp/camera.h>.  The RZ/V CRU emits
# CR10 (packed RAW10, 8x row stride) which v4l2src/videoconvert cannot read;
# this element delivers GRAY8 / GRAY16_LE / NV12 so a camera feeds omxh264enc.
# See docs/v2n-camera-csi.md ("GStreamer source").
#
# Same structure as alp-gpu2d-compose_0.6.bb: the sources come from the alp-sdk
# tag; to build a local checkout set
#   INHERIT += "externalsrc"
#   EXTERNALSRC:pn-gst-plugin-alp-camera = "/path/to/alp-sdk/tools/gst-alpcamerasrc"

SUMMARY     = "alpcamerasrc GStreamer element (CR10 camera to GRAY8/GRAY16_LE/NV12)"
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../LICENSE;md5=787726818c896f394f6627ab59d98d69"

DEPENDS = "alp-sdk gstreamer1.0 gstreamer1.0-plugins-base"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
require recipes-core/alp-sdk/alp-sdk-version.inc
# Links the per-machine libalp_sdk.so, so must not be shared across machines (#2850).
PACKAGE_ARCH = "${MACHINE_ARCH}"

S = "${WORKDIR}/git/tools/gst-alpcamerasrc"

EXTRA_OECMAKE = "-DALP_SDK_VERSION=${PV}"

inherit cmake pkgconfig

# A plugin module: the .so is the payload, not a dev symlink.
FILES:${PN} = "${libdir}/gstreamer-1.0/libgstalpcamera.so"
FILES:${PN}-dev = ""
INSANE_SKIP:${PN} += "dev-so"
