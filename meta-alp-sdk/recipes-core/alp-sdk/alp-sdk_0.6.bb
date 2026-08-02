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

# Regenerate the CMake toolchain file as a do_configure prefunc.
#
# cmake.bbclass sequences its generator as `addtask generate_toolchain_file
# after do_patch before do_configure`, and do_patch is the task's ONLY
# dependency.  CI bakes this recipe under `INHERIT += "externalsrc"`, which
# DELETES do_patch (bb.build.deltask) -- and deltask also strips the deleted
# task out of every other task's deps.  do_generate_toolchain_file therefore
# keeps its `before do_configure` ordering edge but ends up with no task
# dependencies at all, and WORKDIR sits in basehash_ignore_vars, so its stamp
# is a pure function of toolchain variables that never change: once stamped,
# it can never go stale.  externalsrc also sets SSTATE_SKIP_CREATION = "1",
# so there is no sstate to restore ${WORKDIR} from.
#
# do_configure, by contrast, IS forced to re-run on every source change via
# externalsrc's do_configure[file-checksums].  So the moment anything removes
# ${WORKDIR}/toolchain.cmake, the pair desynchronises permanently and every
# subsequent configure dies with:
#     CMake Error ... Could not find toolchain file: <WORKDIR>/toolchain.cmake
#     CMake Error: CMAKE_C_COMPILER not set, after EnableLanguage
#
# Rewriting the file from do_configure is idempotent and self-healing, and is
# poky's own idiom for materialising a ${WORKDIR} input immediately before
# configure (cf. autotools.bbclass, waf.bbclass, recipes-kernel/perf/perf.bb).
do_configure[prefuncs] += "do_generate_toolchain_file"

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

# DRP-AI3 NPU backend (RZ/V2N on-die), default OFF.  Unlike the four
# above this one is NOT a silent degrade and NOT dep-free: when
# ALP_SDK_USE_DRPAI_V2N=ON, src/yocto/inference_drpai.cpp is added to the
# target, #includes <linux/drpai.h> + MeraDrpRuntimeWrapper.h and links
# four vendor libraries.  The -D flags and the build deps therefore have to
# move together, which is why they are routed through one PACKAGECONFIG
# switch.
#
# What this switch supplies -- 2 of the 9 inputs src/yocto/CMakeLists.txt
# looks for:
#   drpai    -> ${includedir}/linux/drpai.h  (meta-rz-drpai, drpai_1.4.0)
#   lib-tvm  -> libtvm_runtime.so            (meta-rz-drpai)
#
# RESIDUAL GAP -- do NOT read this switch as self-contained.  The other
# seven inputs -- MeraDrpRuntimeWrapper.h, the tvm/runtime/profiling.h +
# dlpack/dlpack.h + dmlc/logging.h header tree it hard-includes, and
# libmera2_runtime.so / libmera2_plan_io.so / libdrp_tvm_rt.so -- are
# packaged by NO recipe -- not here, not in meta-rz-drpai.  They exist only
# inside a BUILT rzv_drp-ai_tvm (RUHMI) checkout, headers under apps/ and
# tvm/, libs under obj/build_runtime/v2h/lib (RZ/V2N consumes the v2h
# runtime build; obj/build_runtime/v2m is Renesas RZ/V2M, a different and
# older SoC -- never ours).  ALP_DRPAI_TVM_APPS + CMAKE_LIBRARY_PATH are a
# plain-CMake-only escape hatch and do NOT work here: poky's
# meta/classes-recipe/cmake.bbclass sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
# and CMAKE_FIND_ROOT_PATH_MODE_INCLUDE to ONLY, so find_path()/
# find_library() re-root every search -- HINTS included -- under the
# recipe's own sysroot and silently never try a path outside it.  The
# builder must instead hand-stage the checkout's headers + libs into the
# recipe sysroot (${STAGING_INCDIR} / ${STAGING_LIBDIR}) before enabling
# this PACKAGECONFIG; see meta-alp-sdk/README.md's "Making the RUHMI
# checkout visible to the bake".
#
# Consequence, spelled out because it is SILENT: an enabled bake links
# libalp_sdk.so against those three, so the shipped ELF carries DT_NEEDED
# libmera2_runtime.so, libmera2_plan_io.so and libdrp_tvm_rt.so (all three
# SONAMEs unversioned) that no package in the image provides.  On the
# scarthgap poky in use (DISTRO_VERSION 5.0.11) that is not a build
# failure: an unresolvable DT_NEEDED only emits "Couldn't find shared
# library provider for ..." as a bb.note (poky meta/lib/oe/package.py) and
# records no RDEPENDS, so the file-rdeps QA check has nothing to fire on.
# No INSANE_SKIP is added here: none is required today, and a silent one
# would suppress the single diagnostic that exists.  The failure surfaces
# on target instead, as ld.so refusing to load libalp_sdk.so.
# TODO: package the MERA2 / DRP-AI TVM runtime (an alp-sdk-internal recipe
# over the account-gated prebuilts) and add it to this DEPENDS list; only
# then is `drpai` complete.  Re-check the QA behaviour at the first bake
# that enables it -- no alp-image-edge bake has ever completed on this
# host, with or without DRP-AI, so none of this is bake-verified.
#
# ALP_SDK_DRPAI_REQUIRED rides with the enable: this PACKAGECONFIG is an
# EXPLICIT opt-in, so an unsatisfiable stack must fail do_configure instead
# of warning and dropping the backend.  ALP_SDK_DRPAI_REQUIRED defaults OFF
# in src/yocto/CMakeLists.txt for the OTHER path that can set
# ALP_SDK_USE_DRPAI_V2N -- a builder passing -DALP_SDK_USE_DRPAI_V2N=ON to a
# direct plain-CMake configure by hand, where an incomplete host should
# degrade cleanly rather than hard-fail.  (NOT scripts/alp_orchestrate/
# kconfig.py's capabilities.drp_ai auto-emit: that mechanism does not reach
# this option for any Yocto A55 build today -- see src/yocto/CMakeLists.txt's
# ALP_SDK_DRPAI_REQUIRED option help for why.)
PACKAGECONFIG[drpai]    = "-DALP_SDK_USE_DRPAI_V2N=ON -DALP_SDK_DRPAI_REQUIRED=ON,-DALP_SDK_USE_DRPAI_V2N=OFF -DALP_SDK_DRPAI_REQUIRED=OFF,drpai lib-tvm,"

# Inference backends are NOT build-time dependencies of the SDK library
# by default.  The Yocto build (src/yocto/) links only the
# <alp/inference.h> dispatcher + the portable stubs; the vendor NPU
# backends are compile-gated and stay out unless asked for by name.  The
# DEEPX DX-M1 backend is behind ALP_SDK_USE_DEEPX_DXM1 and compiles
# against an in-tree stub header, so it remains dep-free.  DRP-AI3 is the
# one exception, and it is opted into explicitly: PACKAGECONFIG "drpai"
# above flips the flag and adds the deps in the same switch.
# src/yocto/inference_drpai.cpp is real MeraDrpRuntimeWrapper code -- the
# "NOT_IMPLEMENTED stub / issue #58" description this comment used to
# carry is no longer true -- but it has never run on DRP-AI silicon and
# no full alp-image-edge bake has completed with it enabled, so treat the
# backend as BENCH-UNVERIFIED.
# Where a per-machine NPU userspace runtime package exists it is
# installed by the *image* recipe (see alp-image-edge's
# IMAGE_INSTALL:append:e1m-v2m101 = "dx-rt").  There is still NO
# build-time backend pinning; silicon is the source of truth and apps
# pick per-handle at runtime via alp_inference_open(.backend = ...).

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
