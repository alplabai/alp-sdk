# SPDX-License-Identifier: Apache-2.0
#
# Stages the MERA2 / DRP-AI TVM runtime -- the RESIDUAL GAP named in
# alp-sdk_0.6.bb's PACKAGECONFIG[drpai] comment (#1145).  `drpai` +
# `lib-tvm` (meta-rz-drpai) cover <linux/drpai.h> and
# libtvm_runtime.so; this recipe supplies the rest of what
# src/yocto/CMakeLists.txt probes for -- the header tree, the eight
# prebuilt RUHMI libraries (plus the runtime CLOSURE those need, not
# just the three the link line names directly) -- AND, as of #1145's
# third correction, a NINTH library it does not merely stage but
# actually COMPILES: libmera_drpai_wrapper.so.
#
# THE THIRD GAP -- MeraDrpRuntimeWrapper HAS NO PREBUILT LIBRARY AT ALL.
# `MeraDrpRuntimeWrapper::MeraDrpRuntimeWrapper()` / `Run()` / `SetInput()` /
# `GetInputInfo()` and friends are not vendor-prebuilt symbols; they are
# APPLICATION-SIDE GLUE SOURCE (`apps/MeraDrpRuntimeWrapper.cpp`) that every
# RUHMI sample app compiles itself (see `apps/CMakeLists.txt`: every
# executable target lists the .cpp directly in its own `SRC`).  `nm -D
# --defined-only` across all eight `obj/build_runtime/v2h/lib/*.so` files
# confirms the symbol is in none of them.  A first cut of this recipe staged
# only `MeraDrpRuntimeWrapper.h` -- the DECLARATIONS -- and the gap surfaced
# via the direct plain-CMake path (-DALP_SDK_USE_DRPAI_V2N=ON, which predates
# this recipe) in a downstream consumer (alp-perception) as unresolved
# `MeraDrpRuntimeWrapper::*` symbols in `libalp_sdk.so`, which had built
# "successfully" only because a shared library permits undefined symbols by
# default (see the durable fix: alp-sdk's own CMakeLists.txt now links with
# `-Wl,--no-undefined`, so this class of bug fails at alp-sdk's OWN link
# from here on, not a downstream consumer's).
#
# This recipe now COMPILES `apps/MeraDrpRuntimeWrapper.cpp` into
# `libmera_drpai_wrapper.so` (do_compile, below) rather than leaving it for
# every consumer to compile for itself -- one library, packaged once, is a
# packaging concern and keeps a single copy of the vendor glue, matching how
# this recipe already treats the eight prebuilt libraries.  Compiling it
# needs MORE than the four include dirs a header-only consumer of
# MeraDrpRuntimeWrapper.h needs (apps/, tvm/include, the NESTED
# tvm/3rdparty/dlpack/include, tvm/3rdparty/dmlc-core/include): the .cpp
# itself hard-includes `apps/include/mera_runtime.h` -> `apps/include/rt.h`
# + `apps/include/mera2_runtime_plan/plan_io.h` (the MERA2 in-process
# runtime API), `<spdlog/spdlog.h>` + `<asio.hpp>` (mera_runtime.h), and
# `tvm/3rdparty/compiler-rt/builtin_fp16.h`.  See do_compile for the
# `kDLDrpAi` / `setup/include` trap this uncovered on top of that -- RUHMI's
# own upstream tvm header is missing a Renesas extension the .cpp's ctor
# needs, and a BUILT checkout does not apply that patch to itself.
#
# A first cut of this recipe staged only three libraries
# (libmera2_runtime.so, libmera2_plan_io.so, libdrp_tvm_rt.so) and
# would fail do_package_qa's file-rdeps check: those three DT_NEED five
# more libraries this recipe never
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
# This recipe FETCHES NOTHING and VENDORS NOTHING: the eight prebuilt
# libraries and the DRP-AI Translator are Renesas/EdgeCortix account-gated
# and must never be committed to this public layer.  (`apps/MeraDrpRuntimeWrapper.cpp`
# itself IS plain Apache-2.0 RUHMI source -- see LICENSE, below -- but this
# recipe still never vendors even that: do_compile reads it out of the
# builder's own checkout at build time, same as everything else here.) It
# only STAGES/COMPILES out of a BUILT rzv_drp-ai_tvm (RUHMI) checkout the
# builder already has on disk (RUHMI is itself Apache-2.0 -- <https://
# github.com/renesas-rz/rzv_drp-ai_tvm> -- only the prebuilt MERA2 libs +
# Translator inside a built tree are gated), pointed to by the
# RUHMI_DRPAI_TVM_DIR variable. See docs/bring-up-drpai-v2n.md section 4 for how
# to set it.
#
# RZ/V2N consumes the checkout's v2h runtime build
# (obj/build_runtime/v2h/lib); obj/build_runtime/v2m is Renesas
# RZ/V2M, a different and older SoC -- never ours, despite the name
# collision with the unrelated E1M-V2M (RZ/V2N + DEEPX) SKU.

SUMMARY = "MERA2 / DRP-AI TVM runtime, staged + compiled from a builder-supplied RUHMI checkout"
DESCRIPTION = "Headers (MeraDrpRuntimeWrapper.h + the tvm/dlpack/dmlc-core \
tree it hard-includes) and nine libraries that the alp-sdk DRP-AI3 backend \
(src/yocto/inference_drpai.cpp) links against, directly or transitively: \
eight copied verbatim out of a built rzv_drp-ai_tvm (RUHMI) checkout \
(libmera2_runtime.so, libmera2_plan_io.so, libdrp_tvm_rt.so, libdrp_rt.so, \
libacl_rt.so, libarm_compute.so, libarm_compute_core.so, \
libarm_compute_graph.so), plus a ninth this recipe COMPILES from that same \
checkout's apps/MeraDrpRuntimeWrapper.cpp application-side glue source \
(libmera_drpai_wrapper.so) -- that source is not shipped as a prebuilt by \
RUHMI at all; every consumer is expected to compile it for itself, and this \
recipe does so once so every alp-sdk consumer links one library instead of \
each duplicating the vendor glue.  Sourced from RUHMI_DRPAI_TVM_DIR; this \
recipe carries none of that payload itself."
HOMEPAGE = "https://github.com/renesas-rz/rzv_drp-ai_tvm"

# The RUHMI project itself is Apache-2.0 -- including apps/MeraDrpRuntimeWrapper.cpp,
# which carries no separate license file of its own and falls under the
# repo-root LICENSE -- but what this recipe ALSO packages, the prebuilt
# MERA2 libraries under a BUILT checkout's obj/build_runtime/v2h/lib, comes
# out of Renesas/EdgeCortix's account-gated Translator toolchain and
# carries no redistributable LICENSE file this recipe can checksum.  CLOSED
# is the standard OE idiom for "no shippable license text"; it does not
# assert a license, it declines to.  One recipe, one LICENSE value covering
# everything it packages -- splitting Apache-2.0 (the compiled wrapper) from
# CLOSED (the gated libraries it links against) is not worth a PACKAGES-level
# split for a recipe this small.  Consumers building this recipe already
# hold whatever entitlement got them the checkout in the first place.
LICENSE = "CLOSED"

# No SRC_URI: this recipe never fetches the gated payload, only stages /
# compiles it from a directory the builder already has locally.
SRC_URI = ""
PV = "2.7.0"

S = "${WORKDIR}"

# spdlog + asio are apps/include/mera_runtime.h's own hard dependencies
# (`#include <spdlog/spdlog.h>` / `#include <asio.hpp>` +
# `#include <asio/io_context.hpp>`), pulled in transitively by
# MeraDrpRuntimeWrapper.cpp's `#include "mera_runtime.h"`.  Both are
# reachable without any bblayers.conf change: meta-oe (LAYERDEPENDS_alp-sdk
# = "core openembedded-layer" in meta-alp-sdk's layer.conf) carries
# recipes-support/spdlog and recipes-support/asio.  Header-only for asio
# (no runtime RDEPENDS needed); spdlog builds SPDLOG_BUILD_SHARED, so
# libmera_drpai_wrapper.so DT_NEEDs libspdlog.so -- OE's automatic shlibs
# pass resolves that RDEPENDS on its own (a real build-time DEPENDS + link,
# unlike the RUHMI libraries below, which are copied, not built, and so
# need the explicit RDEPENDS block instead -- see there for why).
DEPENDS = "spdlog asio"

# do_configure: nothing to configure, prebuilt/compiled straight from a
# builder-supplied checkout.  do_compile is a REAL step now (#1145's third
# correction) -- see below.
do_configure[noexec] = "1"

# Set by the builder in local.conf (or the environment, via
# BB_ENV_PASSTHROUGH_ADDITIONS) to a built rzv_drp-ai_tvm checkout's
# root -- e.g. the directory containing apps/, tvm/, and
# obj/build_runtime/v2h/lib/.  Deliberately not a hard default: an
# empty value is exactly what makes do_compile / do_install fail loudly
# below instead of silently compiling nothing / shipping an empty package.
RUHMI_DRPAI_TVM_DIR ?= ""

# Shared by do_compile and do_install so the "unset" case fails identically
# (same message) no matter which task hits it first -- do_compile now needs
# the checkout too, not just do_install.
def _drpai_ruhmi_dir(d):
    import os

    ruhmi_dir = d.getVar("RUHMI_DRPAI_TVM_DIR")
    if not ruhmi_dir:
        bb.fatal(
            "RUHMI_DRPAI_TVM_DIR is unset. Point it at a BUILT "
            "rzv_drp-ai_tvm (RUHMI) checkout's root before building "
            "mera2-drpai-tvm or enabling alp-sdk's PACKAGECONFIG[drpai] "
            "-- see docs/bring-up-drpai-v2n.md section 4."
        )
    return os.path.abspath(ruhmi_dir)

# Compiles apps/MeraDrpRuntimeWrapper.cpp into libmera_drpai_wrapper.so.
#
# Fails loudly (bb.fatal) when the checkout can't supply what this ONE file
# needs to compile -- the same "loud, not silent" posture do_install has
# always had for the header/library staging, now covering the source build
# too.  Deliberately a SEPARATE required-payload check from do_install's:
# this task only needs a subset (the .cpp + its own includes + three of the
# eight libraries it links against), not the full eight-library closure.
python do_compile() {
    import os
    import shutil
    import subprocess

    ruhmi_dir = _drpai_ruhmi_dir(d)
    apps_dir  = os.path.join(ruhmi_dir, "apps")
    lib_dir   = os.path.join(ruhmi_dir, "obj/build_runtime/v2h/lib")
    wrapper_cpp = os.path.join(apps_dir, "MeraDrpRuntimeWrapper.cpp")

    required = [
        ("apps/MeraDrpRuntimeWrapper.cpp", wrapper_cpp, "file"),
        ("apps/include/rt.h", os.path.join(apps_dir, "include/rt.h"), "file"),
        ("apps/include/mera_runtime.h", os.path.join(apps_dir, "include/mera_runtime.h"), "file"),
        ("apps/include/mera2_runtime_plan", os.path.join(apps_dir, "include/mera2_runtime_plan"), "dir"),
        ("tvm/include", os.path.join(ruhmi_dir, "tvm/include"), "dir"),
        ("tvm/3rdparty/dlpack/include", os.path.join(ruhmi_dir, "tvm/3rdparty/dlpack/include"), "dir"),
        ("tvm/3rdparty/dmlc-core/include", os.path.join(ruhmi_dir, "tvm/3rdparty/dmlc-core/include"), "dir"),
        ("tvm/3rdparty/compiler-rt", os.path.join(ruhmi_dir, "tvm/3rdparty/compiler-rt"), "dir"),
        ("setup/include", os.path.join(ruhmi_dir, "setup/include"), "dir"),
        ("libmera2_runtime.so", os.path.join(lib_dir, "libmera2_runtime.so"), "file"),
        ("libmera2_plan_io.so", os.path.join(lib_dir, "libmera2_plan_io.so"), "file"),
        ("libdrp_tvm_rt.so", os.path.join(lib_dir, "libdrp_tvm_rt.so"), "file"),
    ]
    missing = []
    for label, path, kind in required:
        ok = os.path.isdir(path) if kind == "dir" else os.path.isfile(path)
        if not ok:
            missing.append("%s (expected %s at %s)" % (label, kind, path))
    if missing:
        bb.fatal(
            "RUHMI_DRPAI_TVM_DIR=%s is missing payload apps/MeraDrpRuntimeWrapper.cpp "
            "needs to compile:\n  %s\n"
            "If tvm/include looks empty, run `git submodule update --init "
            "--recursive` in the checkout. apps/include/{rt.h,mera_runtime.h,"
            "mera2_runtime_plan/} and setup/include/ ship alongside apps/ and "
            "tvm/ in a normal RUHMI clone; they are not submodules."
            % (ruhmi_dir, "\n  ".join(missing))
        )

    # apps/MeraDrpRuntimeWrapper.cpp:83's `int device_type_{kDLDrpAi};`
    # default member initialiser needs `kDLDrpAi`, an enum value RUHMI's own
    # UPSTREAM tvm/include/tvm/runtime/c_runtime_api.h does not define --
    # it is a Renesas extension (`kDLDrpAi = 36`) that setup/README.md's
    # installation instructions apply with
    # `cp ${TVM_ROOT}/setup/include/*.h ${TVM_ROOT}/tvm/include/tvm/runtime/`
    # as a manual INSTALL-TIME step, not a submodule pin or a build
    # artefact -- a checkout whose obj/build_runtime/v2h/lib/*.so are
    # already built (this recipe's whole precondition) can still carry the
    # pristine, un-patched tvm/include tree, because that cp is a one-way
    # side effect of following the install doc, not something `git status`
    # would ever flag as missing. Confirmed empirically compiling this file
    # against the untouched header: fails with "'kDLDrpAi' was not declared
    # in this scope"; copying setup/include over a COPY of tvm/include's
    # tvm/runtime/ fixes it, exactly matching RUHMI's own install step.
    #
    # Never mutate the builder's own checkout in place -- RUHMI_DRPAI_TVM_DIR
    # may be reused across bakes and by the builder's own tooling outside
    # this recipe -- so the overlay lands in a private WORKDIR copy instead.
    patched_tvm_inc = os.path.join(d.getVar("WORKDIR"), "tvm_include_patched")
    if os.path.isdir(patched_tvm_inc):
        shutil.rmtree(patched_tvm_inc)
    shutil.copytree(os.path.join(ruhmi_dir, "tvm/include"), patched_tvm_inc)
    setup_inc = os.path.join(ruhmi_dir, "setup/include")
    for name in os.listdir(setup_inc):
        shutil.copy2(os.path.join(setup_inc, name),
                     os.path.join(patched_tvm_inc, "tvm/runtime", name))

    out_so = os.path.join(d.getVar("B"), "libmera_drpai_wrapper.so")
    cmd = (d.getVar("CXX") or "").split()
    cmd += (d.getVar("CXXFLAGS") or "").split()
    cmd += [
        "-std=c++17", "-fPIC", "-shared",
        # Matches RUHMI's own apps/CMakeLists.txt build of this exact file
        # (its V2H/V2N branch): -DMERA_DRP_RUNTIME selects the MERA
        # (1.x/2.0) runtime path mera_runtime.h expects; -DKDLDRPAI picks
        # this ctor's real-DRP-AI default (vs. the CPU-fallback KDLCPUMODE
        # branch), matching every non-CPU-fallback vendor sample.
        "-DMERA_DRP_RUNTIME", "-DKDLDRPAI",
        # SPDLOG_FMT_EXTERNAL: meta-oe builds spdlog against an external fmt
        # (spdlog_1.13.0.bb: DEPENDS = "fmt", EXTRA_OECMAKE +=
        # "-DSPDLOG_FMT_EXTERNAL=on"), so the bundled fmt copy is never
        # installed.  The shipped spdlog/fmt/fmt.h still takes its bundled
        # branch unless the CONSUMER defines this too, giving
        #     spdlog/fmt/fmt.h:28:14: fatal error:
        #     spdlog/fmt/bundled/core.h: No such file or directory
        # on a bake without it.  Defining it selects the "#include <fmt/core.h>"
        # branch instead, which fmt (already in the sysroot via spdlog's own
        # DEPENDS) provides.  RUHMI's apps/CMakeLists.txt does not need this
        # because it builds against a vendored spdlog with fmt bundled in.
        "-DSPDLOG_FMT_EXTERNAL",
        "-I" + apps_dir,
        "-I" + os.path.join(apps_dir, "include"),
        "-I" + patched_tvm_inc,
        "-I" + os.path.join(ruhmi_dir, "tvm/3rdparty/dlpack/include"),
        "-I" + os.path.join(ruhmi_dir, "tvm/3rdparty/dmlc-core/include"),
        "-I" + os.path.join(ruhmi_dir, "tvm/3rdparty/compiler-rt"),
        wrapper_cpp,
        "-o", out_so,
        "-Wl,-soname,libmera_drpai_wrapper.so",
    ]
    cmd += (d.getVar("LDFLAGS") or "").split()
    cmd += [
        "-L" + lib_dir, "-Wl,-rpath-link," + lib_dir,
        # mera2_runtime / mera2_plan_io / drp_tvm_rt: staged by do_install
        # into THIS package's own libdir, but not yet there at do_compile
        # time -- link straight against the checkout's copies, same as the
        # header overlay above. tvm_runtime is deliberately NOT linked here:
        # it comes from the separate, optional meta-rz-drpai `lib-tvm`
        # recipe, and this recipe must not hard-DEPENDS on that layer (it
        # is a soft LAYERRECOMMENDS on purpose -- AEN/NX91 have no DRP-AI
        # silicon at all).  Any tvm::runtime::* reference this .cpp's
        # ImplDrpTvm class leaves unresolved here is resolved later, at
        # alp_sdk's OWN final link, which already links tvm_runtime
        # directly (src/yocto/CMakeLists.txt's ALP_SDK_USE_DRPAI_V2N
        # block) -- exactly the transitive-resolution shape --no-undefined
        # (the durable fix, alp-sdk's own CMakeLists.txt) is designed to
        # still permit: it only checks alp_sdk's OWN objects' undefined
        # refs against ITS full link line, not what an intermediate .so
        # left unresolved in ITS OWN build.
        "-lmera2_runtime", "-lmera2_plan_io", "-ldrp_tvm_rt",
        "-lspdlog", "-lpthread",
    ]
    bb.note("mera2-drpai-tvm: compiling %s -> %s" % (wrapper_cpp, out_so))
    subprocess.run(cmd, check=True, cwd=d.getVar("B"))
}

# Fails do_install loudly -- not an empty package -- when the payload
# is absent or incomplete, per each of: the var unset, the checkout
# missing entirely, the tvm/ submodule left uninitialised (the header
# tree is present but empty), or a v2m build pointed at by mistake.
python do_install() {
    import os
    import shutil

    ruhmi_dir = _drpai_ruhmi_dir(d)

    # (label, path relative to RUHMI_DRPAI_TVM_DIR, kind)
    #
    # All eight obj/build_runtime/v2h/lib/*.so files are staged, not just
    # the three src/yocto/inference_drpai.cpp links against directly: the
    # other five are DT_NEEDED by those three (Arm Compute Library +
    # DRP runtime), and a real bake fails do_package_qa's file-rdeps check
    # the moment any of them is missing from the image.  (The ninth
    # library, libmera_drpai_wrapper.so, is do_compile's own output --
    # validated by that task, not re-validated here.)
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

    # Same setup/include overlay do_compile applies privately to compile
    # against (see that task's comment for the kDLDrpAi / RUHMI
    # install-doc background) -- repeated here so a header CONSUMER of
    # this package's sysroot sees the same patched tvm/runtime/*.h RUHMI's
    # own runtime was actually built against, not the pristine upstream
    # copy the checkout still carries on disk.
    setup_inc = os.path.join(ruhmi_dir, "setup/include")
    if os.path.isdir(setup_inc):
        for name in os.listdir(setup_inc):
            shutil.copy2(os.path.join(setup_inc, name),
                         os.path.join(incdir, "tvm/runtime", name))

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

    # The ninth library is OURS, not RUHMI's: do_compile builds it from
    # apps/MeraDrpRuntimeWrapper.cpp against the payload above (#1145).
    shutil.copy2(
        os.path.join(d.getVar("B"), "libmera_drpai_wrapper.so"),
        os.path.join(libdir, "libmera_drpai_wrapper.so"),
    )
}

# All nine libraries ship with no DT_SONAME, so they belong in the main
# runtime package, not -dev -- but merely appending to FILES:${PN} (the
# first cut of this recipe) is NOT enough to get them there: bitbake's
# default PACKAGES order lists ${PN}-dev ahead of ${PN}, and
# ${PN}-dev's own built-in FILES default already globs "${libdir}/*.so"
# ahead of anything FILES:${PN} claims, so every one of these libraries
# was landing in mera2-drpai-tvm-dev regardless -- exactly the
# do_package_qa "non-symlink .so ... in ... -dev" error that produces.
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
    ${libdir}/libmera_drpai_wrapper.so \
"
FILES:${PN}-dev = " \
    ${includedir}/MeraDrpRuntimeWrapper.h \
    ${includedir}/tvm \
    ${includedir}/dlpack \
    ${includedir}/dmlc \
"

# ldflags: three of the eight RUHMI-prebuilt libraries carry no .gnu.hash
# section, so insane.bbclass's ldflags check fires:
#     QA Issue: File /usr/lib/libarm_compute_graph.so in package
#     mera2-drpai-tvm doesn't have GNU_HASH (didn't pass LDFLAGS?)
#
# This one IS a false positive, unlike the file-rdeps errors above it, which
# were real and are fixed by staging the full closure rather than skipped.
# The check exists to catch OUR builds dropping the distro LDFLAGS; these
# three are vendor prebuilts we only copy, so there are no LDFLAGS to have
# dropped. Verified against the untouched RUHMI payload with `readelf -S`:
# exactly libarm_compute.so, libarm_compute_core.so and
# libarm_compute_graph.so ship without .gnu.hash (they are ARM Compute
# Library prebuilts); the other five copied libraries do have it. So
# packaging is not mangling them; they arrive this way. The ninth library,
# libmera_drpai_wrapper.so, IS one of OUR builds (do_compile does append
# ${LDFLAGS} to its link line), so it is not expected to need this skip --
# but the check is package-wide, not per-file, so this stays in place
# regardless.
#
# Consequence, stated because it is not free: without .gnu.hash those three
# fall back to the older SysV .hash chain, which makes symbol lookup slower at
# load time. That is a vendor property of the binaries, not something this
# recipe can fix, and it does not affect correctness.
INSANE_SKIP:${PN} += "ldflags"

# libmmngr.so.1 / libmmngrbuf.so.1 are DT_NEEDED by libdrp_rt.so /
# libmera2_runtime.so but live outside RUHMI entirely -- they come from
# meta-rz-drpai's mmngr-user-module / mmngrbuf-user-module recipes.
# do_package_qa's file-rdeps check has no way to infer that on its own
# (nothing in this recipe DEPENDS on them at build time), so it has to
# be said explicitly or those QA errors recur.  (This
# does NOT apply to libmera_drpai_wrapper.so's own runtime deps -- spdlog
# is a real DEPENDS, so shlibs infers that RDEPENDS automatically; see the
# DEPENDS comment above.)
#
# kernel-module-mmngr is added too, even though nothing here links
# against it (kernel modules never show up as a DT_NEEDED entry, so QA
# never asks for it): mmngr-user-module / mmngrbuf-user-module are thin
# ioctl wrappers around /dev/mmngr, which that kernel module provides,
# so the userspace libs are inert without it at runtime. alp-image-common.inc
# also installs it explicitly (ALP_RZ_DRPAI_INSTALL, worked around
# because meta-rz-drpai only hooks core-image-% images) -- listing it
# here too is a harmless duplicate for that image and makes this recipe
# runtime-correct standalone, for any other image that pulls it in.
RDEPENDS:${PN} += "mmngr-user-module mmngrbuf-user-module kernel-module-mmngr"

# Excluded from `bitbake world`: this recipe only builds successfully
# once a builder has pointed RUHMI_DRPAI_TVM_DIR at a real checkout,
# same posture as recipes-deepx/dx-rt for the other license-gated NPU
# runtime in this layer.
EXCLUDE_FROM_WORLD = "1"

# EXCLUDE_FROM_WORLD only keeps this out of `bitbake world`.  It does NOT
# stop an image, or a PACKAGECONFIG DEPENDS, pulling it into a machine that
# has no DRP-AI at all -- so scope it explicitly.  The payload staged here
# is the RZ/V2N `obj/build_runtime/v2h` prebuilt set; on an AEN or NX9101
# build it is not merely useless, it is wrong.
COMPATIBLE_MACHINE = "^(e1m-v2n101-a55|e1m-v2n102-a55|e1m-v2m101-a55|e1m-v2m102-a55)$"

# Pin to MACHINE_ARCH.  With the default TUNE_PKGARCH this recipe's output
# would share an sstate/feed slot with every other aarch64 machine, so a
# V2N-specific prebuilt could be served to an unrelated aarch64 build.  The
# staged libraries are SoC-specific, not tune-specific.
PACKAGE_ARCH = "${MACHINE_ARCH}"

# BENCH-UNVERIFIED, and more unverified than the packaging fix this entry
# previously described: that one was static staging (copy files, fix
# FILES:/RDEPENDS), corrected twice by inspection against the real
# checkout's contents. This one
# ADDS A COMPILE STEP -- a real g++ invocation against RUHMI's real headers
# was run by hand on a dev host to prove apps/MeraDrpRuntimeWrapper.cpp
# actually compiles to a valid .o with every one of the previously-missing
# symbols defined (nm confirms MeraDrpRuntimeWrapper::{ctor,Run,SetInput,
# GetInputInfo} as global T symbols) against the real RUHMI checkout's
# headers plus Ubuntu's spdlog/asio packages standing in for meta-oe's --
# but the FINAL LINK against the real (ARM aarch64) obj/build_runtime/v2h
# libraries could not be exercised on that x86_64 host ("skipping
# incompatible ... when searching for -lmera2_runtime" is an
# architecture mismatch, not a symbol error).
#
# THAT FINAL STEP HAS NOT BEEN TAKEN.  This recipe is correct on paper and
# nothing more.
#
# An earlier revision of this comment claimed a `drpai`-ENABLED
# alp-image-edge bake had completed (12118 tasks, DT_NEEDED resolved, 0
# unresolved symbols, ten libraries in the rootfs).  That claim was removed
# rather than softened, for two reasons:
#
#   1. docs/bring-up-drpai-v2n.md, in the same change, states that no
#      bitbake run of this recipe -- with or without do_compile -- has
#      happened at all, and that the 12118-task bake it refers to ran with
#      `drpai` OFF.  Same task count, opposite verdict.  Both cannot be
#      true.
#   2. The bake it described could not have run: it names
#      `PACKAGECONFIG:append:pn-alp-sdk = " drpai"`, and until #1145 no
#      PACKAGECONFIG[drpai] existed in alp-sdk_0.6.bb.  OE errors out on an
#      append naming an undefined flag.
#
# So what is actually established, and nothing beyond it: do_compile
# cross-compiles apps/MeraDrpRuntimeWrapper.cpp on an x86_64 host up to the
# final link, where it stops with "skipping incompatible ... when searching
# for -lmera2_runtime" -- an architecture mismatch against the aarch64
# obj/build_runtime/v2h libraries, not a symbol error.  Whether packaging
# passes do_package_qa, and whether the symbols resolve against the real
# aarch64 payload, are both UNTESTED.
#
# The kernel side is proven independently of this recipe: /dev/drpai0 probes
# clean on a real board and DRPAI_GET_DRPAI_AREA returns the 0xD0000000 /
# 512 MiB arena.  That is the vendor driver, not this recipe's payload, and
# it says nothing about the userspace runtime packaged here.
