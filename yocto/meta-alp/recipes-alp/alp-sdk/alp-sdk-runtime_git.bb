# SPDX-License-Identifier: Apache-2.0
#
# Yocto runtime package for the ALP SDK Linux backend.
#
# Builds libalp_sdk.so + the public <alp/...> headers, gated for
# Linux-side use (V2N, V2N-M1, i.MX 93 Linux side).  The Zephyr
# RT-core side ships through Zephyr's module system, NOT through
# Yocto.

SUMMARY     = "ALP SDK Linux runtime"
DESCRIPTION = "Shared library + headers exposing the <alp/...> public surface \
to Yocto applications.  Routes to Linux-side backends (V4L2 for camera, \
ALSA for audio, BlueZ for BLE, etc.)."
HOMEPAGE    = "https://github.com/alplabai/alp-sdk"
LICENSE     = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.1.0+git${SRCPV}"

S = "${WORKDIR}/git"

inherit cmake pkgconfig

DEPENDS = "mbedtls"

# Per-machine inference backend selection drives optional deps.
# alp-inference auto-detects at runtime, but if the build pins a
# default the matching runtime must be installed.
DEPENDS:append:e1m-x-v2n     = " drpai-driver"
DEPENDS:append:e1m-x-v2n-m1  = " drpai-driver dxm1-runtime"
DEPENDS:append:e1m-n93       = " ethosu-driver-library"

EXTRA_OECMAKE = " \
    -DALP_OS=yocto \
    -DALP_HAS_MBEDTLS=ON \
"

EXTRA_OECMAKE:append:e1m-x-v2n     = " -DALP_INFERENCE_BACKEND=drpai"
EXTRA_OECMAKE:append:e1m-x-v2n-m1  = " -DALP_INFERENCE_BACKEND=deepx"
EXTRA_OECMAKE:append:e1m-n93       = " -DALP_INFERENCE_BACKEND=ethosu"

FILES:${PN}     = "${libdir}/libalp_sdk.so.*"
FILES:${PN}-dev = "${includedir}/alp/ \
                   ${libdir}/libalp_sdk.so \
                   ${libdir}/cmake/AlpSdk/ \
                   ${libdir}/pkgconfig/alp-sdk.pc"

# Yocto-side QA: nothing in here should look like a Zephyr build
# artefact.  Bail loudly if it does.
INSANE_SKIP:${PN} = ""
