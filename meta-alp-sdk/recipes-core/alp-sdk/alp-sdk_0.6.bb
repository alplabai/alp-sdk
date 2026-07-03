# SPDX-License-Identifier: Apache-2.0
#
# Builds + installs the Alp SDK runtime (libalp_sdk.so + the
# `<alp/*>` headers) onto the target rootfs.

SUMMARY = "Alp SDK runtime for V2N + V2N-M1 SoMs"
DESCRIPTION = "Cross-platform peripheral + inference + IoT API \
               for the E1M open-standard SoM family.  Provides \
               libalp_sdk.so + <alp/*> headers used by every \
               higher-layer Alp Lab application."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=787726818c896f394f6627ab59d98d69"

# Track the alp-sdk default branch; CI repins SRCREV to the release-tag
# commit when alp-sdk tags a new release (same pattern as the other
# alp-* recipes in this layer, e.g. alp-chips_0.6.bb).
SRC_URI = "git://github.com/alplabai/alp-sdk.git;protocol=https;branch=main"
SRCREV  = "${AUTOREV}"
PV      = "0.6.0"

S = "${WORKDIR}/git"

inherit cmake

# alp-sdk's repo-root CMakeLists.txt builds the plain-CMake
# shared-library variant for Yocto consumers.  Zephyr-only
# integrations bypass this recipe and consume the SDK as a Zephyr
# module via west.yml instead.
EXTRA_OECMAKE = "-DALP_SDK_BUILD_SHARED=ON      \
                 -DALP_SDK_BUILD_EXAMPLES=OFF   \
                 -DALP_OS=yocto"

# Optional Linux-userspace backends (#33 registry migration).  The SDK's
# CMake auto-detects each library via pkg_check_modules and silently
# degrades the class to its priority-0 sw_fallback backend when the
# library is missing from the sysroot -- so WITHOUT these build deps the
# produced libalp_sdk.so would quietly ship without the real MQTT /
# security / audio+I2S / RPC backends.  PACKAGECONFIG makes the choice
# explicit and default-on; images that must shrink can strip entries.
# No cmake -D flags are needed (detection is pkg-config-side), hence the
# empty enable/disable slots.
#   mqtt     -> mosquitto  (meta-openembedded/meta-networking)
#   security -> openssl    (oe-core)
#   audio    -> alsa-lib   (oe-core; also enables the I2S backend)
#   rpc      -> open-amp + libmetal (meta-openamp; default OFF because
#               the layer is not in the standard alp bblayers set yet)
PACKAGECONFIG ??= "mqtt security audio"
PACKAGECONFIG[mqtt]     = ",,mosquitto"
PACKAGECONFIG[security] = ",,openssl"
PACKAGECONFIG[audio]    = ",,alsa-lib"
PACKAGECONFIG[rpc]      = ",,open-amp libmetal"

# Inference backends are NOT build-time dependencies of the SDK
# library.  The Yocto build (src/yocto/) links only the
# <alp/inference.h> dispatcher + the portable stubs; the vendor NPU
# backends are gated/aspirational (DRP-AI3 is a NOT_IMPLEMENTED stub
# today -- issue #58; the DEEPX DX-M1 backend is behind
# ALP_SDK_USE_DEEPX_DXM1 and compiles against an in-tree stub header).
# Where a per-machine NPU userspace runtime package exists it is
# installed by the *image* recipe (see alp-image-edge's
# IMAGE_INSTALL:append:e1m-v2m101 = "dx-rt"), and the DRP-AI3 userspace
# headers come from meta-rz-drpai via the sysroot -- neither is pulled
# as an SDK build dep.  There is NO build-time backend pinning;
# silicon is the source of truth and apps pick per-handle at runtime
# via alp_inference_open(.backend = ...).

FILES:${PN}     += "${libdir}/libalp_sdk.so.*"
FILES:${PN}-dev += "${libdir}/libalp_sdk.so    \
                    ${includedir}/alp/*.h      \
                    ${includedir}/alp/chips/*.h"

# Runtime library deps (libc, the libgcc_s package, and libstdc++ when
# the C++ DEEPX backend is compiled in) are derived automatically by
# OE's shlibs packaging step from what libalp_sdk.so actually links --
# no manual RDEPENDS needed, and the OE package names differ from the
# Debian "libgcc-s" spelling.  DEEPX runtime is a separate recipe;
# OPTIGA Trust M is the only crypto chip whose driver lands here
# unconditionally.

BBCLASSEXTEND = "native nativesdk"
