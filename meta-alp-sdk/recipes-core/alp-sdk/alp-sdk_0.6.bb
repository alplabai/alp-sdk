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

inherit cmake pkgconfig

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

# SoM-declared ALP_INFERENCE_BACKEND_AUTO accelerator order (comma list, best
# first, e.g. "deepx_dxm1,drpai,cpu").  Generated from the SoM preset
# `inference.auto_order` into local.conf by `--emit yocto-conf`; empty means no preset order.
ALP_SDK_INFERENCE_AUTO_ORDER ?= ""
EXTRA_OECMAKE:append = "${@' -DALP_SDK_INFERENCE_AUTO_ORDER=' + d.getVar('ALP_SDK_INFERENCE_AUTO_ORDER') if d.getVar('ALP_SDK_INFERENCE_AUTO_ORDER') else ''}"

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
#   drpai    -> mera2-drpai-tvm + drpai (RZ/V2N on-die DRP-AI3
#               NPU; default OFF).  BOTH are needed, and the flag sets
#               ALP_SDK_DRPAI_REQUIRED=ON, so a missing one is a configure
#               error rather than a silently backend-less library:
#               src/yocto/CMakeLists.txt probes <linux/drpai.h> (from
#               meta-rz-drpai's `drpai`) and the MERA2 closure (mera2-drpai-tvm).  Requires
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
# DRP-AI3 NPU backend (RZ/V2N on-die), default OFF.  Unlike the four
# above this one is NOT a silent degrade and NOT dep-free: when
# ALP_SDK_USE_DRPAI_V2N=ON, src/yocto/inference_drpai.cpp is added to the
# target, #includes <linux/drpai.h> + MeraDrpRuntimeWrapper.h and links
# four vendor libraries.  The -D flags and the build deps therefore have to
# move together, which is why they are routed through one PACKAGECONFIG
# switch.
#
# What this switch supplies -- all 9 inputs src/yocto/CMakeLists.txt
# looks for, across two recipes:
#   drpai            -> ${includedir}/linux/drpai.h  (meta-rz-drpai, drpai_1.4.0)
#   mera2-drpai-tvm  -> MeraDrpRuntimeWrapper.h, the tvm/runtime/profiling.h +
#                       dlpack/dlpack.h + dmlc/logging.h header tree it
#                       hard-includes, libmera2_runtime.so / libmera2_plan_io.so /
#                       libdrp_tvm_rt.so, AND libmera_drpai_wrapper.so --
#                       COMPILED (not staged) by that recipe from the same
#                       checkout's apps/MeraDrpRuntimeWrapper.cpp, since
#                       MeraDrpRuntimeWrapper's own symbols (ctor, Run,
#                       SetInput, GetInputInfo, ...) are application-side
#                       glue source RUHMI ships with no prebuilt library at
#                       all, not one of the eight vendor .so's
#                       (meta-alp-sdk/recipes-renesas/mera2-drpai-tvm)
#
# `mera2-drpai-tvm` closes what used to be a RESIDUAL GAP here: those
# seven inputs used to be packaged by NO recipe -- not here, not in
# meta-rz-drpai -- and existed only inside a BUILT rzv_drp-ai_tvm (RUHMI)
# checkout, headers under apps/ and tvm/, libs under
# obj/build_runtime/v2h/lib (RZ/V2N consumes the v2h runtime build;
# obj/build_runtime/v2m is Renesas RZ/V2M, a different and older SoC --
# never ours).  ALP_DRPAI_TVM_APPS + CMAKE_LIBRARY_PATH remain a
# plain-CMake-only hint and do NOT work under BitBake: poky's
# meta/classes-recipe/cmake.bbclass sets CMAKE_FIND_ROOT_PATH_MODE_LIBRARY
# and CMAKE_FIND_ROOT_PATH_MODE_INCLUDE to ONLY, so find_path()/
# find_library() re-root every search -- HINTS included -- under the
# recipe's own sysroot and silently never try a path outside it.  What
# actually works, and what mera2-drpai-tvm does, is stage/compile the
# checkout's headers + libs into ITS OWN sysroot (${STAGING_INCDIR} /
# ${STAGING_LIBDIR}) so the unmodified probes find them there; see
# meta-alp-sdk/README.md's "Model compilation toolchain (RUHMI / DRP-AI
# TVM)" section and mera2-drpai-tvm's RUHMI_DRPAI_TVM_DIR variable.
#
# The DT_NEEDED consequence that used to be silent -- libalp_sdk.so
# carrying libmera2_runtime.so / libmera2_plan_io.so / libdrp_tvm_rt.so
# (all three SONAMEs unversioned) with no package in the image
# providing them -- is now closed, but it took more than those three
# libraries: they in turn DT_NEED five more (libdrp_rt.so, libacl_rt.so,
# libarm_compute.so, libarm_compute_core.so, libarm_compute_graph.so,
# all also present in RUHMI's obj/build_runtime/v2h/lib/) plus
# libmmngr.so.1 / libmmngrbuf.so.1, which are not RUHMI's to ship at all
# -- they come from meta-rz-drpai's mmngr-user-module /
# mmngrbuf-user-module recipes. mera2-drpai-tvm now stages all eight
# RUHMI libraries in its main package (so OE's automatic shlibs pass
# picks up their DT_NEEDED entries) and RDEPENDS on the two mmngr
# packages explicitly, since nothing DEPENDS-time links against them for
# shlibs to infer the RDEPENDS on its own. A first cut of this recipe
# staged only the three libraries named above and shipped them into the
# wrong package besides (bitbake's default ${PN}-dev file-glob claims
# unversioned *.so before an appended FILES:${PN} sees them) -- a real
# `bitbake -c package_qa` with this PACKAGECONFIG enabled is what caught
# both gaps; static inspection alone had missed them.
#
# THIRD correction, from a real downstream-consumer link failure (not
# `package_qa` this time -- link-complete but symbol-incomplete gets past
# it): staging MeraDrpRuntimeWrapper.h was never enough. Its symbols are
# not in any of the eight libraries above; they are RUHMI's own
# application-side glue SOURCE (apps/MeraDrpRuntimeWrapper.cpp), which
# every RUHMI sample compiles for itself.  libalp_sdk.so linked
# "successfully" with PACKAGECONFIG[drpai] enabled while carrying
# unresolved `MeraDrpRuntimeWrapper::*` references -- invisible at ITS OWN
# link (a shared library permits undefined symbols by default) -- and the
# break only surfaced when alp-perception linked against it downstream.
# mera2-drpai-tvm's do_compile now compiles that source into a ninth
# library, libmera_drpai_wrapper.so, and this file's CMakeLists.txt
# find_library()s + links it exactly like the other four.  THE DURABLE
# FIX rides alongside it: alp-sdk's top-level CMakeLists.txt now links
# libalp_sdk.so with `-Wl,--no-undefined`, so a future gap in this shape
# fails loudly at alp-sdk's OWN link step, not a downstream consumer's.
#
# ALP_SDK_DRPAI_REQUIRED rides with the enable: this PACKAGECONFIG is an
# EXPLICIT opt-in, so an unsatisfiable stack must fail do_configure instead
# of warning and dropping the backend.  ALP_SDK_DRPAI_REQUIRED defaults OFF
# in src/yocto/CMakeLists.txt for the OTHER path that can set
# ALP_SDK_USE_DRPAI_V2N -- a builder passing -DALP_SDK_USE_DRPAI_V2N=ON to a
# direct plain-CMake configure by hand, where an incomplete host should
# degrade cleanly rather than hard-fail.  (NOT scripts/alp_orchestrate/
# kconfig.py's capabilities.drp_ai auto-emit: that mechanism DOES reach a
# real, gate-tested slice today -- buildplan.py calls _slice_cmake_args
# only for os == "baremetal", and test_project_backends.py already
# asserts E1M-V2M101 / E1M-V2N101 a55_cluster baremetal emitting
# -DALP_SDK_USE_DRPAI_V2N=ON.  What keeps THIS recipe's Yocto CMakeLists.txt
# out of that path is simpler: the top-level CMakeLists.txt does
# add_subdirectory(src/${ALP_OS}), so an os: baremetal slice parses
# src/baremetal/CMakeLists.txt and never opens src/yocto/CMakeLists.txt at
# all -- and separately, ALP_SDK_DRPAI_REQUIRED itself is emitted by
# NOTHING in the tree (kconfig.py emits only the USE flag), so REQUIRED
# can never be auto-flipped ON regardless of which slice is building.)
PACKAGECONFIG[drpai]    = "-DALP_SDK_USE_DRPAI_V2N=ON -DALP_SDK_DRPAI_REQUIRED=ON,-DALP_SDK_USE_DRPAI_V2N=OFF -DALP_SDK_DRPAI_REQUIRED=OFF,drpai mera2-drpai-tvm mmngr-user-module mmngrbuf-user-module,mera2-drpai-tvm alp-drpai-udev"

# Warn when the image will carry the DRP-AI driver + runtime (ALP_ENABLE_DRPAI)
# but this SDK build has no DRP-AI backend because RUHMI is not configured:
# `.backend = ALP_INFERENCE_BACKEND_DRPAI` would then fail with NOSUPPORT.
python () {
    if ('rzv2n-family' in (d.getVar('MACHINEOVERRIDES') or '').split(':')
            and d.getVar('ALP_ENABLE_DRPAI') == '1'
            and not d.getVar('RUHMI_DRPAI_TVM_DIR')
            and 'drpai' not in (d.getVar('PACKAGECONFIG') or '').split()):
        bb.warn("alp-sdk: ALP_ENABLE_DRPAI = \"1\" but RUHMI_DRPAI_TVM_DIR is not "
                "set, so libalp_sdk is built WITHOUT the DRP-AI backend "
                "(alp_inference_open(.backend = ALP_INFERENCE_BACKEND_DRPAI) "
                "returns ALP_ERR_NOSUPPORT). Set RUHMI_DRPAI_TVM_DIR to a "
                "RUHMI rzv_drp-ai_tvm checkout, see docs/bring-up-drpai-v2n.md.")
}

# deepx-dxm1 -> dx-rt (DEEPX's own meta-deepx-m1 layer; see
#               conf/machine/include/e1m-v2m-deepx.inc).  Same
#               REQUIRED-flag shape as `drpai` above: turning this on
#               makes src/yocto/inference_deepx.cpp compile against the
#               real dx_rt headers/libdxrt instead of degrading to the
#               portable stub.  NOT default-on unconditionally -- only
#               when the MACHINE actually carries the DEEPX silicon
#               (MACHINE_FEATURES `deepx-dxm1`, set by the V2M machine
#               confs) AND ALP_ENABLE_DEEPX_DXM1 == "1" -- which the V2M
#               machine include defaults on whenever DEEPX's
#               meta-deepx-m1 layer is in bblayers.conf (#482).  A V2M
#               build without that layer (or with the flag forced to
#               "0") still builds -- it links only the dispatcher +
#               portable stub.
# DRP-AI3 backend: auto-enabled on an RZ/V2N-family MACHINE that has the
# DRP-AI node on (ALP_ENABLE_DRPAI, default-on with meta-rz-drpai) AND a
# RUHMI checkout configured (RUHMI_DRPAI_TVM_DIR, which mera2-drpai-tvm
# needs to build the MERA2 runtime).  Without RUHMI it stays off rather
# than failing the bake; set PACKAGECONFIG:append:pn-alp-sdk = " drpai"
# to force it, or ALP_ENABLE_DRPAI = "0" to keep it out.
PACKAGECONFIG:append = "${@' drpai' if ('rzv2n-family' in (d.getVar('MACHINEOVERRIDES') or '').split(':') and d.getVar('ALP_ENABLE_DRPAI') == '1' and d.getVar('RUHMI_DRPAI_TVM_DIR')) else ''}"

# gles -> GPU backend for <alp/gpu2d.h> (src/backends/gpu2d/yocto_gles.c):
#         fill/blit/blend on the Mali-G31 through the vendor EGL + GLES 3
#         stack.  The userspace driver is the Renesas AI SDK's
#         meta-rz-graphics `mali-library` (virtual/egl + virtual/libgles2,
#         egl.pc/glesv2.pc); it is licence-gated and never copied into this
#         layer -- Alp-built images take it from the private mirror, and
#         ALP_ENABLE_GPU2D_GLES = "0" opts out.  REQUIRED rides with the
#         enable (same shape as `drpai`): a bake that asked for the GPU
#         backend must not silently ship a CPU-only libalp_sdk.
#         Auto-on only when the GPU stack is really there: a `mali-family`
#         MACHINE with `opengles` in DISTRO_FEATURES, which meta-rz-graphics
#         (rz-graphics.inc) sets in DISTRO_FEATURES -- so a build without that
#         layer keeps the CPU fallback and does not fail.  :class-target keeps
#         the -native/-nativesdk variants (BBCLASSEXTEND below) off the GPU stack.
#         BENCH-UNVERIFIED.
#         Runtime: the Mali `wayland` userspace variant needs weston running
#         (IMAGE_FEATURES += "alp-display"); without it open() degrades to the
#         CPU path.
PACKAGECONFIG[gles]     = "-DALP_SDK_USE_GPU2D_GLES=ON -DALP_SDK_GPU2D_GLES_REQUIRED=ON,-DALP_SDK_USE_GPU2D_GLES=OFF,virtual/libgles2 virtual/egl,libegl libgles2"
ALP_ENABLE_GPU2D_GLES ?= "1"
PACKAGECONFIG:append:class-target = "${@' gles' if ('mali-family' in (d.getVar('MACHINEOVERRIDES') or '').split(':') and 'opengles' in (d.getVar('DISTRO_FEATURES') or '').split() and d.getVar('ALP_ENABLE_GPU2D_GLES') == '1') else ''}"

# ort -> ONNX Runtime CPU floor (own recipe, recipes-devtools/onnxruntime).
#       REQUIRED rides with the enable, same shape as `drpai`: a missing ORT
#       stack must fail the bake, not silently drop the CPU backend.  Auto-on
#       only where the MACHINE conf sets ALP_ENABLE_ORT_CPU = "1" (V2M101/
#       V2M102/V2M103/V2N101/V2N102/V2N103, #1259).  With the DEEPX runtime
#       on, dx-rt ships its own libonnxruntime (1.20.1), so ALP_ORT_PKG
#       points DEPENDS/RDEPENDS at that one and the layer's onnxruntime is
#       not built or installed (same libonnxruntime.so files would collide).
#       RDEPENDS puts libonnxruntime in the image.  Never reaches resolve_auto() ahead of an NPU backend.
ALP_ORT_PKG = "${@'libonnxruntime' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else 'onnxruntime'}"
PACKAGECONFIG[ort]      = "-DALP_SDK_USE_ORT_CPU=ON -DALP_SDK_ORT_REQUIRED=ON,-DALP_SDK_USE_ORT_CPU=OFF,${ALP_ORT_PKG},${ALP_ORT_PKG}"
PACKAGECONFIG:append = "${@' ort' if d.getVar('ALP_ENABLE_ORT_CPU') == '1' else ''}"

PACKAGECONFIG[deepx-dxm1] = "-DALP_SDK_USE_DEEPX_DXM1=ON -DALP_SDK_DEEPX_REQUIRED=ON,-DALP_SDK_USE_DEEPX_DXM1=OFF,dx-rt,"
PACKAGECONFIG:append = "${@bb.utils.contains('MACHINE_FEATURES', 'deepx-dxm1', ' deepx-dxm1' if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' else '', '', d)}"

# Fail loudly when the DEEPX opt-in is set but DEEPX's layer is not in
# bblayers.conf. Without this the same misconfiguration surfaces as a
# cryptic "Nothing PROVIDES dx-rt" here and "Nothing RPROVIDES dx-driver"
# at do_rootfs.
python () {
    if d.getVar('ALP_ENABLE_DEEPX_DXM1') == '1' and 'meta-deepx-m1' not in (d.getVar('BBFILE_COLLECTIONS') or '').split():
        bb.fatal("ALP_ENABLE_DEEPX_DXM1 = \"1\" but the meta-deepx-m1 layer is "
                 "not in bblayers.conf. Add DEEPX's meta-deepx-m1 layer "
                 "(see conf/machine/include/e1m-v2m-deepx.inc) or unset the opt-in.")
}

# Inference backends are NOT build-time dependencies of the SDK
# library.  The Yocto build (src/yocto/) links only the
# <alp/inference.h> dispatcher + the portable stubs; the vendor NPU
# backends are gated (the DRP-AI3 backend is real MeraDrpRuntimeWrapper
# code since #1145, but compiles in only under the `drpai` PACKAGECONFIG
# above and has run on DRP-AI3 silicon via a cross-built SDK (#1268), not
# yet from a baked image; the DEEPX DX-M1 backend
# (src/yocto/inference_deepx.cpp) is real dx_rt-API code and compiles in
# only under the `deepx-dxm1` PACKAGECONFIG below -- #482 wired that
# PACKAGECONFIG + its auto-enable, not the backend body itself -- and
# it has run on DX-M1 silicon (#1262), auto-enabled only on a MACHINE
# that carries `deepx-dxm1` in MACHINE_FEATURES with
# ALP_ENABLE_DEEPX_DXM1 = "1").
# No `drpai`-enabled alp-image-edge bake has completed yet, and no
# `bitbake` run of mera2-drpai-tvm_2.7.0.bb -- with or without
# `do_compile` -- has happened at all; see docs/bring-up-drpai-v2n.md
# section 4 and mera2-drpai-tvm_2.7.0.bb for exactly what IS and is NOT
# established (a hand-run g++ against RUHMI's real headers on an x86_64
# dev host proved MeraDrpRuntimeWrapper.cpp compiles clean with every
# needed symbol defined; the final aarch64 link, packaging QA and
# symbol resolution against the real payload are all UNTESTED). Treat
# the backend as BENCH-UNVERIFIED.
# Where a per-machine NPU userspace runtime package exists it is
# installed by the *image* recipe (DEEPX's dx-rt/dx-driver are opted in
# per the e1m-v2m10{1,2,3}-a55 MACHINE confs via
# conf/machine/include/e1m-v2m-deepx.inc, gated on
# ALP_ENABLE_DEEPX_DXM1); this recipe's own `deepx-dxm1` PACKAGECONFIG
# only pulls the `dx-rt` BUILD dependency (headers + libdxrt), not the
# runtime install.
#
# DRP-AI3 is the exception, and only when PACKAGECONFIG[drpai] is on: its
# DEPENDS field names `drpai` explicitly.  That is required,
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

# The DRP-AI lock directory (/run/alp) is created by alp-drpai-udev, which
# PACKAGECONFIG[drpai] pulls in; non-DRP-AI images get no such directory.

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
