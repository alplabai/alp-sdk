# SPDX-License-Identifier: Apache-2.0
#
# Recipe to build the alp-sdk LVGL dashboard example for the
# E1M-X V2N MIPI-DSI panel.
#
# Follows the same structure as alp-edgeai_0.6.bb.

SUMMARY     = "ALP SDK LVGL dashboard example for the E1M-X MIPI-DSI panel"
DESCRIPTION = "Minimal LVGL 9 dashboard rendered via the Linux DRM/KMS pipeline \
on the RK055HDMIPI4MA0 panel (720x1280).  Demonstrates the DRM backend, \
double-buffered scanout, and optional evdev touch input."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

DEPENDS = "lvgl libdrm"
RDEPENDS:${PN} = "lvgl"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

S = "${WORKDIR}/git/examples/display/lvgl-dashboard-x-evk"

inherit cmake

EXTRA_OECMAKE = "-DALP_OS=yocto"

FILES:${PN} = "${bindir}/alp-lvgl-dashboard"
