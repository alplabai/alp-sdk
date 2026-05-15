# SPDX-License-Identifier: Apache-2.0
#
# Builds + installs the ALP SDK runtime (libalp_sdk.so + the
# `<alp/*>` headers) onto the target rootfs.

SUMMARY = "ALP SDK runtime for V2N + V2N-M1 SoMs"
DESCRIPTION = "Cross-platform peripheral + inference + IoT API \
               for the E1M open-standard SoM family.  Provides \
               libalp_sdk.so + <alp/*> headers used by every \
               higher-layer ALP Lab application."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=86d3f3a95c324c9479bd8986968f4327"

# Pin the v0.5 release tag.  CI bumps this when alp-sdk tags a
# new release; meta-alp-sdk's tag tracks alp-sdk's.
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "950a428000000000000000000000000000000000"
# TBD: pin to a release-tag commit after the v0.5 tag lands.

PV = "0.5.0"

S = "${WORKDIR}/git"

inherit cmake

# alp-sdk's repo-root CMakeLists.txt builds the plain-CMake
# shared-library variant for Yocto consumers.  Zephyr-only
# integrations bypass this recipe and consume the SDK as a Zephyr
# module via west.yml instead.
EXTRA_OECMAKE = "-DALP_SDK_BUILD_SHARED=ON      \
                 -DALP_SDK_BUILD_EXAMPLES=OFF   \
                 -DALP_OS=yocto"

# Per-machine inference backend selection drives optional deps.
# alp-inference auto-detects at runtime, but if the build pins a
# default the matching runtime must be installed.  These overrides
# fire on the historical (un-suffixed) MACHINEOVERRIDES strings;
# the -a55 cluster MACHINEs prepend those onto MACHINEOVERRIDES.
DEPENDS:append:e1m-v2n101 = " drpai-driver"
DEPENDS:append:e1m-v2n102 = " drpai-driver"
DEPENDS:append:e1m-v2m101 = " drpai-driver dxm1-runtime"
DEPENDS:append:e1m-v2m102 = " drpai-driver dxm1-runtime"
DEPENDS:append:e1m-nx9101 = " ethosu-driver-library"

EXTRA_OECMAKE:append:e1m-v2n101 = " -DALP_INFERENCE_BACKEND=drpai"
EXTRA_OECMAKE:append:e1m-v2n102 = " -DALP_INFERENCE_BACKEND=drpai"
EXTRA_OECMAKE:append:e1m-v2m101 = " -DALP_INFERENCE_BACKEND=deepx"
EXTRA_OECMAKE:append:e1m-v2m102 = " -DALP_INFERENCE_BACKEND=deepx"
EXTRA_OECMAKE:append:e1m-nx9101 = " -DALP_INFERENCE_BACKEND=ethosu"

FILES:${PN}     += "${libdir}/libalp_sdk.so.*"
FILES:${PN}-dev += "${libdir}/libalp_sdk.so    \
                    ${includedir}/alp/*.h      \
                    ${includedir}/alp/chips/*.h"

# DEEPX runtime is a separate recipe; OPTIGA Trust M is the only
# crypto chip whose driver lands here unconditionally.
RDEPENDS:${PN} = "libstdc++ libgcc-s"

BBCLASSEXTEND = "native nativesdk"
