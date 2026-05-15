# SPDX-License-Identifier: Apache-2.0
#
# Recipe to build the alp-sdk EdgeAI reference application.
#
# Reference path rebased from the v0.1 layout
# (examples/edgeai-vision-aen) to the v0.6 layout
# (examples/aen/edgeai-vision-aen) after the AEN-family example
# move.

SUMMARY     = "ALP SDK EdgeAI reference application"
DESCRIPTION = "End-to-end EdgeAI demo: camera capture → ISP / format-convert \
→ Ethos-U / DRP-AI inference → results overlay on display."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://../../../LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

DEPENDS = "alp-sdk"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

# Rebased v0.6 path: AEN-family demos moved under examples/aen/.
S = "${WORKDIR}/git/examples/aen/edgeai-vision-aen"

inherit cmake

EXTRA_OECMAKE = "-DALP_OS=yocto"

FILES:${PN} = "${bindir}/alp-edgeai"

# AEN A32-class targets are deferred to v0.7; on V2N / V2M / NX9101
# the example builds for the A55 cluster MACHINE and exercises the
# Ethos-U / DRP-AI / DEEPX backend matching the SoM.
