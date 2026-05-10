# SPDX-License-Identifier: Apache-2.0
#
# Recipe to build the ALP SDK as a shared library on Yocto.
# v0.1: skeleton.  do_compile + do_install land in v0.4 when the
# Linux backend in src/yocto/ goes from stub to real.

SUMMARY     = "ALP SDK — unification SDK for E1M edge AI modules"
DESCRIPTION = "Provides libalp_sdk.so and the <alp/...> headers for \
applications targeting Yocto-based E1M variants (V2N, V2N-M1, i.MX 93)."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

# Track main during pre-1.0; pin to a tag for production builds.
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.1.0+git${SRCPV}"

S = "${WORKDIR}/git"

inherit cmake

EXTRA_OECMAKE = "-DALP_OS=yocto"

# v0.4 fills in:
#   FILES:${PN}     = "${libdir}/libalp_sdk.so.*"
#   FILES:${PN}-dev = "${includedir}/alp/* ${libdir}/libalp_sdk.so ${libdir}/cmake/AlpSdk/*"
