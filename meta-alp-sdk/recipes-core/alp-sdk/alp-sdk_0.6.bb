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

# zcbor is a hard build dependency, not a PACKAGECONFIG opt-in like mqtt/
# security/audio above: this recipe stages it unconditionally, so a
# sysroot missing it is a packaging bug, not a degrade candidate.
# ALP_SDK_MODEL_ZCBOR_REQUIRED=ON below is what turns
# src/yocto/CMakeLists.txt's zcbor find_path/find_library block (#1254)
# into a FATAL_ERROR on that sysroot instead of a warn-and-degrade to
# stub_model.c's ALP_ERR_NOSUPPORT body (stub_model.c stays in that
# file's source list -- it is still the degrade path for a plain-CMake
# -DALP_OS=yocto build made WITHOUT this flag, e.g.
# pr-plain-cmake.yml's stock-Ubuntu `yocto` leg).  Provided by this
# layer's own recipes-devtools/zcbor/zcbor_0.9.1.bb.
DEPENDS += "zcbor"

inherit cmake

# alp-sdk's repo-root CMakeLists.txt builds the plain-CMake
# shared-library variant for Yocto consumers.  Zephyr-only
# integrations bypass this recipe and consume the SDK as a Zephyr
# module via west.yml instead.
#
# ALP_SDK_MODEL_ZCBOR_REQUIRED=ON (#1254 follow-up): without this flag
# a sysroot that loses the zcbor DEPENDS above would silently degrade
# to stub_model.c instead of failing configure -- shipping a silently
# ALP_ERR_NOSUPPORT libalp_sdk.so, the exact regression this recipe
# exists to prevent.
EXTRA_OECMAKE = "-DALP_SDK_BUILD_SHARED=ON            \
                 -DALP_SDK_BUILD_EXAMPLES=OFF         \
                 -DALP_OS=yocto                       \
                 -DALP_SDK_MODEL_ZCBOR_REQUIRED=ON"

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
#   drpai    -> mera2-drpai-tvm + drpai + lib-tvm (RZ/V2N on-die DRP-AI3
#               NPU; default OFF).  All THREE are needed, and the flag sets
#               ALP_SDK_DRPAI_REQUIRED=ON, so a missing one is a configure
#               error rather than a silently backend-less library:
#               src/yocto/CMakeLists.txt probes <linux/drpai.h> (from
#               meta-rz-drpai's `drpai`), libtvm_runtime (its `lib-tvm`),
#               and the MERA2 closure (mera2-drpai-tvm).  Requires
#               meta-rz-drpai in bblayers.conf.  Turning this on is what
#               makes src/yocto/inference_drpai.cpp compile in -- see #1145.
#               Note this recipe carries BBCLASSEXTEND = "native nativesdk"
#               while mera2-drpai-tvm does not; append the flag with a
#               :pn-alp-sdk qualifier so a native variant does not resolve
#               a nonexistent mera2-drpai-tvm-native.
PACKAGECONFIG ??= "mqtt security audio"
PACKAGECONFIG[mqtt]     = ",,mosquitto"
PACKAGECONFIG[security] = ",,openssl"
PACKAGECONFIG[audio]    = ",,alsa-lib"
PACKAGECONFIG[rpc]      = ",,open-amp libmetal"
PACKAGECONFIG[drpai]    = "-DALP_SDK_USE_DRPAI_V2N=ON -DALP_SDK_DRPAI_REQUIRED=ON,-DALP_SDK_USE_DRPAI_V2N=OFF,mera2-drpai-tvm drpai lib-tvm,"

# Inference backends are NOT build-time dependencies of the SDK
# library.  The Yocto build (src/yocto/) links only the
# <alp/inference.h> dispatcher + the portable stubs; the vendor NPU
# backends are gated (the DRP-AI3 backend is real MeraDrpRuntimeWrapper
# code since #1145, but compiles in only under the `drpai` PACKAGECONFIG
# above and has never run on DRP-AI silicon; the DEEPX DX-M1 backend is
# behind ALP_SDK_USE_DEEPX_DXM1 and compiles against an in-tree stub
# header).
# Where a per-machine NPU userspace runtime package exists it is
# installed by the *image* recipe (DEEPX's dx-rt is opted in per the
# e1m-v2m10{1,2}-a55 MACHINE confs, gated on ALP_ENABLE_DEEPX_DXM1 --
# see alp-image-common.inc's DEEPX note); this recipe pulls the DEEPX
# runtime not at all.
#
# DRP-AI3 is the exception, and only when PACKAGECONFIG[drpai] is on: its
# DEPENDS field names `drpai` and `lib-tvm` explicitly.  That is required,
# not belt-and-braces.  The userspace HEADERS (<linux/drpai.h>) do NOT
# "come from meta-rz-drpai via the sysroot" merely by that layer being in
# bblayers.conf -- meta-rz-drpai ships them through its own
# core-image-%.bbappend's TOOLCHAIN_TARGET_TASK entry, which (like its
# RDEPENDS payload) never matches an alp-image-* recipe name (issue
# #1176).  alp-image-common.inc ports that entry so `populate_sdk`
# produces the header for SDK consumers, but that does nothing for THIS
# recipe's own do_configure -- hence the explicit build deps.
#
# With the flag OFF (the default) no DRP-AI build dep is pulled and the
# Yocto build links only the dispatcher + portable stubs.  There is NO
# build-time backend pinning either way; silicon is the source of truth
# and apps pick per-handle at runtime via alp_inference_open(.backend =
# ...).

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
