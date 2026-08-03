# SPDX-License-Identifier: Apache-2.0
#
# Stages the MERA2 / DRP-AI TVM runtime -- the RESIDUAL GAP named in
# alp-sdk_0.6.bb's PACKAGECONFIG[drpai] comment (#1145).  `drpai` +
# `lib-tvm` (meta-rz-drpai) cover <linux/drpai.h> and
# libtvm_runtime.so; this recipe supplies the other seven of the
# nine inputs src/yocto/CMakeLists.txt probes for -- the header tree,
# plus the runtime CLOSURE those headers need, not just the three
# libraries the link line names directly.
#
# A first cut of this recipe staged only three libraries
# (libmera2_runtime.so, libmera2_plan_io.so, libdrp_tvm_rt.so) and
# failed a real `bitbake -c package_qa` with PACKAGECONFIG[drpai]
# enabled: those three DT_NEED five more libraries this recipe never
# staged (libdrp_rt.so, libacl_rt.so, libarm_compute.so,
# libarm_compute_core.so, libarm_compute_graph.so -- all present in
# the same RUHMI obj/build_runtime/v2h/lib/ directory) and two more
# (libmmngr.so.1, libmmngrbuf.so.1) that come from outside RUHMI
# entirely, via meta-rz-drpai's mmngr-user-module /
# mmngrbuf-user-module recipes.  This recipe now stages all eight
# RUHMI libraries and RDEPENDS on the two mmngr packages -- see the
# `required` list and the RDEPENDS block below.
#
# obj/build_runtime/v2h/lib/ also carries three non-library files --
# log_out.bin, softmax_out.bin, split_out.bin -- deliberately left
# unstaged: they are RUHMI's own sample-output fixtures (compared
# against a reference during RUHMI's own test run), not shared
# libraries any DT_NEEDED entry or CMake probe references, so shipping
# them here would just be dead weight in the image.
#
# This recipe FETCHES NOTHING and VENDORS NOTHING: the libraries and
# headers are Renesas/EdgeCortix account-gated prebuilts that must
# never be committed to this public layer.  It only STAGES them out
# of a BUILT rzv_drp-ai_tvm (RUHMI) checkout the builder already has
# on disk (RUHMI is itself Apache-2.0 -- <https://github.com/renesas-rz/
# rzv_drp-ai_tvm> -- only the prebuilt MERA2 libs + Translator inside
# a built tree are gated), pointed to by the RUHMI_DRPAI_TVM_DIR
# variable. See meta-alp-sdk/README.md's "Making the RUHMI checkout
# visible to the bake" for how to set it.
#
# RZ/V2N consumes the checkout's v2h runtime build
# (obj/build_runtime/v2h/lib); obj/build_runtime/v2m is Renesas
# RZ/V2M, a different and older SoC -- never ours, despite the name
# collision with the unrelated E1M-V2M (RZ/V2N + DEEPX) SKU.

SUMMARY = "MERA2 / DRP-AI TVM runtime, staged from a builder-supplied RUHMI checkout"
DESCRIPTION = "Headers (MeraDrpRuntimeWrapper.h + the tvm/dlpack/dmlc-core \
tree it hard-includes) and libraries (libmera2_runtime.so, \
libmera2_plan_io.so, libdrp_tvm_rt.so, libdrp_rt.so, libacl_rt.so, \
libarm_compute.so, libarm_compute_core.so, libarm_compute_graph.so) that \
the alp-sdk DRP-AI3 backend (src/yocto/inference_drpai.cpp) links against, \
directly or transitively.  Copied verbatim out of a built rzv_drp-ai_tvm \
(RUHMI) checkout at RUHMI_DRPAI_TVM_DIR; this recipe carries none of that \
payload itself."
HOMEPAGE = "https://github.com/renesas-rz/rzv_drp-ai_tvm"

# The RUHMI project itself is Apache-2.0, but what this recipe actually
# packages -- the prebuilt MERA2 libraries under a BUILT checkout's
# obj/build_runtime/v2h/lib -- comes out of Renesas/EdgeCortix's
# account-gated Translator toolchain and carries no redistributable
# LICENSE file this recipe can checksum.  CLOSED is the standard OE
# idiom for "no shippable license text"; it does not assert a license,
# it declines to.  Consumers building this recipe already hold whatever
# entitlement got them the checkout in the first place.
LICENSE = "CLOSED"

# No SRC_URI: this recipe never fetches the gated payload, only stages
# it from a directory the builder already has locally.
SRC_URI = ""
PV = "2.7.0"

S = "${WORKDIR}"

# Prebuilt payload -- nothing to configure or compile.
do_configure[noexec] = "1"
do_compile[noexec] = "1"

# Set by the builder in local.conf (or the environment, via
# BB_ENV_PASSTHROUGH_ADDITIONS) to a built rzv_drp-ai_tvm checkout's
# root -- e.g. the directory containing apps/, tvm/, and
# obj/build_runtime/v2h/lib/.  Deliberately not a hard default: an
# empty value is exactly what makes do_install fail loudly below
# instead of silently shipping an empty package.
RUHMI_DRPAI_TVM_DIR ?= ""

# Fails do_install loudly -- not an empty package -- when the payload
# is absent or incomplete, per each of: the var unset, the checkout
# missing entirely, the tvm/ submodule left uninitialised (the header
# tree is present but empty), or a v2m build pointed at by mistake.
python do_install() {
    import os
    import shutil

    ruhmi_dir = d.getVar("RUHMI_DRPAI_TVM_DIR")
    if not ruhmi_dir:
        bb.fatal(
            "RUHMI_DRPAI_TVM_DIR is unset. Point it at a BUILT "
            "rzv_drp-ai_tvm (RUHMI) checkout's root -- see "
            "meta-alp-sdk/README.md's 'Making the RUHMI checkout "
            "visible to the bake' -- before building mera2-drpai-tvm "
            "or enabling alp-sdk's PACKAGECONFIG[drpai]."
        )
    ruhmi_dir = os.path.abspath(ruhmi_dir)

    # (label, path relative to RUHMI_DRPAI_TVM_DIR, kind)
    #
    # All eight obj/build_runtime/v2h/lib/*.so files are staged, not just
    # the three src/yocto/inference_drpai.cpp links against directly: the
    # other five are DT_NEEDED by those three (Arm Compute Library +
    # DRP runtime), and a real bake fails do_package_qa's file-rdeps check
    # the moment any of them is missing from the image.
    required = [
        ("MeraDrpRuntimeWrapper.h", "apps/MeraDrpRuntimeWrapper.h", "file"),
        ("tvm/include", "tvm/include", "dir"),
        ("tvm/3rdparty/dlpack/include", "tvm/3rdparty/dlpack/include", "dir"),
        ("tvm/3rdparty/dmlc-core/include", "tvm/3rdparty/dmlc-core/include", "dir"),
        ("libmera2_runtime.so", "obj/build_runtime/v2h/lib/libmera2_runtime.so", "file"),
        ("libmera2_plan_io.so", "obj/build_runtime/v2h/lib/libmera2_plan_io.so", "file"),
        ("libdrp_tvm_rt.so", "obj/build_runtime/v2h/lib/libdrp_tvm_rt.so", "file"),
        ("libdrp_rt.so", "obj/build_runtime/v2h/lib/libdrp_rt.so", "file"),
        ("libacl_rt.so", "obj/build_runtime/v2h/lib/libacl_rt.so", "file"),
        ("libarm_compute.so", "obj/build_runtime/v2h/lib/libarm_compute.so", "file"),
        ("libarm_compute_core.so", "obj/build_runtime/v2h/lib/libarm_compute_core.so", "file"),
        ("libarm_compute_graph.so", "obj/build_runtime/v2h/lib/libarm_compute_graph.so", "file"),
    ]
    missing = []
    for label, rel, kind in required:
        full = os.path.join(ruhmi_dir, rel)
        ok = os.path.isfile(full) if kind == "file" else os.path.isdir(full)
        if not ok:
            missing.append("%s (expected %s at %s)" % (label, kind, full))
    if missing:
        bb.fatal(
            "RUHMI_DRPAI_TVM_DIR=%s is missing required RUHMI payload:\n  %s\n"
            "If tvm/include or the 3rdparty dirs look empty, run "
            "`git submodule update --init --recursive` in the checkout -- "
            "tvm/ is a submodule and ships uninitialised on a bare clone. "
            "Confirm the checkout is a BUILT v2h runtime (RZ/V2N); "
            "obj/build_runtime/v2m is the older, different Renesas RZ/V2M "
            "and is never staged by this recipe."
            % (ruhmi_dir, "\n  ".join(missing))
        )

    incdir = d.getVar("D") + d.getVar("includedir")
    libdir = d.getVar("D") + d.getVar("libdir")
    bb.utils.mkdirhier(incdir)
    bb.utils.mkdirhier(libdir)

    # Headers flatten to the include root so the unmodified CMake
    # find_path() probes (which search for e.g. "MeraDrpRuntimeWrapper.h"
    # and "tvm/runtime/profiling.h" with no PATH_SUFFIXES) resolve once
    # this recipe's sysroot is staged.
    shutil.copy2(
        os.path.join(ruhmi_dir, "apps/MeraDrpRuntimeWrapper.h"),
        os.path.join(incdir, "MeraDrpRuntimeWrapper.h"),
    )
    for rel in (
        "tvm/include",
        "tvm/3rdparty/dlpack/include",
        "tvm/3rdparty/dmlc-core/include",
    ):
        shutil.copytree(os.path.join(ruhmi_dir, rel), incdir, dirs_exist_ok=True)

    for lib in (
        "libmera2_runtime.so",
        "libmera2_plan_io.so",
        "libdrp_tvm_rt.so",
        "libdrp_rt.so",
        "libacl_rt.so",
        "libarm_compute.so",
        "libarm_compute_core.so",
        "libarm_compute_graph.so",
    ):
        shutil.copy2(
            os.path.join(ruhmi_dir, "obj/build_runtime/v2h/lib", lib),
            os.path.join(libdir, lib),
        )
}

# All eight libraries ship with no DT_SONAME, so they belong in the main
# runtime package, not -dev -- but merely appending to FILES:${PN} (the
# first cut of this recipe) is NOT enough to get them there: bitbake's
# default PACKAGES order lists ${PN}-dev ahead of ${PN}, and
# ${PN}-dev's own built-in FILES default already globs "${libdir}/*.so"
# ahead of anything FILES:${PN} claims, so every one of these libraries
# was landing in mera2-drpai-tvm-dev regardless -- exactly the
# do_package_qa "non-symlink .so ... in ... -dev" error a real bake hit.
# Redefining (not appending to) FILES:${PN}-dev below drops that default
# glob; this recipe never ships a .la/.a/.pc/cmake file, so headers-only
# is a complete definition, not a narrowed one. With no .so left for
# -dev to claim, insane.bbclass's dev-so QA check no longer fires, so no
# INSANE_SKIP is needed either.
FILES:${PN} += " \
    ${libdir}/libmera2_runtime.so \
    ${libdir}/libmera2_plan_io.so \
    ${libdir}/libdrp_tvm_rt.so \
    ${libdir}/libdrp_rt.so \
    ${libdir}/libacl_rt.so \
    ${libdir}/libarm_compute.so \
    ${libdir}/libarm_compute_core.so \
    ${libdir}/libarm_compute_graph.so \
"
FILES:${PN}-dev = " \
    ${includedir}/MeraDrpRuntimeWrapper.h \
    ${includedir}/tvm \
    ${includedir}/dlpack \
    ${includedir}/dmlc \
"

# libmmngr.so.1 / libmmngrbuf.so.1 are DT_NEEDED by libdrp_rt.so /
# libmera2_runtime.so but live outside RUHMI entirely -- they come from
# meta-rz-drpai's mmngr-user-module / mmngrbuf-user-module recipes.
# do_package_qa's file-rdeps check has no way to infer that on its own
# (nothing in this recipe DEPENDS on them at build time), so it has to
# be said explicitly or the QA errors from the real bake recur.
#
# kernel-module-mmngr is added too, even though nothing here links
# against it (kernel modules never show up as a DT_NEEDED entry, so QA
# never asks for it): mmngr-user-module / mmngrbuf-user-module are thin
# ioctl wrappers around /dev/mmngr, which that kernel module provides,
# so the userspace libs are inert without it at runtime. alp-image-edge.bb
# also installs it explicitly (ALP_DRPAI_IMAGE_INSTALL, worked around
# because meta-rz-drpai only hooks core-image-% images) -- listing it
# here too is a harmless duplicate for that image and makes this recipe
# runtime-correct standalone, for any other image that pulls it in.
RDEPENDS:${PN} += "mmngr-user-module mmngrbuf-user-module kernel-module-mmngr"

# Excluded from `bitbake world`: this recipe only builds successfully
# once a builder has pointed RUHMI_DRPAI_TVM_DIR at a real checkout,
# same posture as recipes-deepx/dx-rt for the other license-gated NPU
# runtime in this layer.
EXCLUDE_FROM_WORLD = "1"

# BENCH-UNVERIFIED: DRP-AI has never run on silicon. A full alp-image-edge
# bake HAS completed on this host with PACKAGECONFIG[drpai] OFF (the base
# image); with it ON, the do_package_qa fix above is code-complete but has
# not yet been proven by a green bake -- the packaging bug it fixes was
# found by an actual `bitbake` run, not by inspection, so treat this as
# fixed-on-paper until the next drpai-enabled bake confirms it.
