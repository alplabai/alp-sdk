# SPDX-License-Identifier: Apache-2.0
#
# Stages the MERA2 / DRP-AI TVM runtime -- the RESIDUAL GAP named in
# alp-sdk_0.6.bb's PACKAGECONFIG[drpai] comment (#1145).  `drpai` +
# `lib-tvm` (meta-rz-drpai) cover <linux/drpai.h> and
# libtvm_runtime.so; this recipe supplies the other seven of the
# nine inputs src/yocto/CMakeLists.txt probes for:
#
#   MeraDrpRuntimeWrapper.h, the tvm/dlpack/dmlc-core header tree it
#   hard-includes, and libmera2_runtime.so / libmera2_plan_io.so /
#   libdrp_tvm_rt.so.
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
libmera2_plan_io.so, libdrp_tvm_rt.so) that the alp-sdk DRP-AI3 backend \
(src/yocto/inference_drpai.cpp) links against.  Copied verbatim out of a \
built rzv_drp-ai_tvm (RUHMI) checkout at RUHMI_DRPAI_TVM_DIR; this recipe \
carries none of that payload itself."
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
    required = [
        ("MeraDrpRuntimeWrapper.h", "apps/MeraDrpRuntimeWrapper.h", "file"),
        ("tvm/include", "tvm/include", "dir"),
        ("tvm/3rdparty/dlpack/include", "tvm/3rdparty/dlpack/include", "dir"),
        ("tvm/3rdparty/dmlc-core/include", "tvm/3rdparty/dmlc-core/include", "dir"),
        ("libmera2_runtime.so", "obj/build_runtime/v2h/lib/libmera2_runtime.so", "file"),
        ("libmera2_plan_io.so", "obj/build_runtime/v2h/lib/libmera2_plan_io.so", "file"),
        ("libdrp_tvm_rt.so", "obj/build_runtime/v2h/lib/libdrp_tvm_rt.so", "file"),
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

    for lib in ("libmera2_runtime.so", "libmera2_plan_io.so", "libdrp_tvm_rt.so"):
        shutil.copy2(
            os.path.join(ruhmi_dir, "obj/build_runtime/v2h/lib", lib),
            os.path.join(libdir, lib),
        )
}

# All three libraries ship with no DT_SONAME (matches the DT_NEEDED
# entries alp-sdk_0.6.bb's RESIDUAL GAP comment names as "unversioned"),
# so they belong in the main runtime package, not -dev, and trip
# insane.bbclass's dev-so QA check for exactly that reason.
INSANE_SKIP:${PN} += "dev-so"

FILES:${PN} += " \
    ${libdir}/libmera2_runtime.so \
    ${libdir}/libmera2_plan_io.so \
    ${libdir}/libdrp_tvm_rt.so \
"
FILES:${PN}-dev += " \
    ${includedir}/MeraDrpRuntimeWrapper.h \
    ${includedir}/tvm \
    ${includedir}/dlpack \
    ${includedir}/dmlc \
"

# Excluded from `bitbake world`: this recipe only builds successfully
# once a builder has pointed RUHMI_DRPAI_TVM_DIR at a real checkout,
# same posture as recipes-deepx/dx-rt for the other license-gated NPU
# runtime in this layer.
EXCLUDE_FROM_WORLD = "1"

# BENCH-UNVERIFIED: this recipe is static-checked only. DRP-AI has
# never run on silicon and no full alp-image-edge bake has ever
# completed on this host, with or without PACKAGECONFIG[drpai] enabled.
