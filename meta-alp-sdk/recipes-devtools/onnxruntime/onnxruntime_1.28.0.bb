# SPDX-License-Identifier: Apache-2.0
#
# ONNX Runtime -- the CPU inference backend for the Cortex-A55 Linux side.
#
# WHY THIS EXISTS AS OUR OWN RECIPE.  Neither openembedded-core nor
# meta-openembedded ships an ONNX Runtime recipe, and the two vendor stacks
# that do ship FORKS at incompatible versions: nxp-imx builds
# gitsm://github.com/nxp-imx/onnxruntime-imx.git at 1.24.3 (carrying the
# neutron + vsinpu execution providers), meta-renesas-ai ships 1.8.0.
# Inheriting either would make metadata/libraries/onnxruntime.yaml's single
# `version:` field untrue on one family or the other the moment a second
# family enabled it. This recipe builds UPSTREAM at one version so the
# manifest stays honest as Alp Lab adds SoC families -- see
# docs/superpowers/plans/2026-08-05-onnxruntime-own-recipe.md for the full
# O(families) vs O(1) "scaling argument"; the maintainer chose this path
# explicitly on 2026-08-05 over the meta-imx-ml fallback.
#
# alp-sdk does NOT consume ORT's execution-provider mechanism at all -- NPU
# dispatch happens one level up, in src/yocto/inference_yocto.c's
# resolve_auto(), which routes to the Ethos-U / DRP-AI / DEEPX backends
# ahead of CPU.  The vendor forks above exist mainly to add EPs (neutron,
# vsinpu); this recipe's whole job is the CPU floor, which upstream serves
# exactly as well as any fork, without inheriting the fork's version skew.
#
# It does NOT vendor sources (ADR 0018 non-goal: no in-tree vendors/ tree --
# do_fetch pulls upstream at build time, this recipe carries no payload of
# its own) and does NOT fork upstream (ADR 0017: ride over the vendor/
# upstream SDK, do not reimplement it) -- packaging only. Lands Tier B
# (ADR 0018): recipe-only, not built in alp-sdk's per-PR CI, selectable from
# a customer's board.yaml via `libraries: [onnxruntime]` once
# metadata/libraries/onnxruntime.yaml (a separate commit) exists.
#
# ---------------------------------------------------------------------
# PINS (resolved 2026-08-05, transcribed verbatim -- do not re-derive):
#   ORT_TAG            v1.28.0 (latest release, published 2026-07-25)
#   ORT_SHA             da9b5e364c465de65c49d91e696cd6485270757f
#   LICENSE LICENSE.md5 0f7e3b1308cb5c00b372a6e78835732d
# ---------------------------------------------------------------------
#
# LICENSE IS COMPOUND, NOT JUST "MIT".  Upstream's own repo LICENSE file is
# plain MIT (Microsoft Corporation).  But onnx/onnx (the model-format
# parser cmake/onnxruntime_external_deps.cmake FetchContent-declares as
# `onnx`, staged below by FETCHCONTENT_SOURCE_DIR_ONNX) is a required build
# dependency -- there is no CPU-only build that excludes it -- and onnx/onnx
# is Apache-2.0.  So libonnxruntime.so as shipped here genuinely contains
# Apache-2.0 code; declaring plain MIT would be false about the
# redistributed artifact.  ThirdPartyNotices.txt is 325 KB at this tag,
# confirming this is not a lone edge case.  Maintainer legal sign-off for
# the compound SPDX expression (and the matching
# metadata/schemas/library-v1.schema.json enum extension, a separate
# commit) was given 2026-08-05.
#
# Both halves are pinned below, not just the MIT one. The onnx/onnx LICENSE
# md5 (3b83ef96387f14655fc854ddc3c6bd57) was computed for real off the
# actual v1.22.0 tree (unzipped v1.22.0.zip from cmake/deps.txt,
# `md5sum LICENSE`, confirmed Apache License Version 2.0), not guessed or
# carried over from another package. Without this second entry, an
# upstream relicense of onnx/onnx would ship silently -- LIC_FILES_CHKSUM
# only re-checks the paths it's given.
LICENSE = "MIT & Apache-2.0"
LIC_FILES_CHKSUM = "file://LICENSE;md5=0f7e3b1308cb5c00b372a6e78835732d \
    file://../deps/onnx/onnx-1.22.0/LICENSE;md5=3b83ef96387f14655fc854ddc3c6bd57"

DESCRIPTION = "ONNX Runtime -- cross-platform inference engine"
HOMEPAGE = "https://onnxruntime.ai"

# nobranch=1, NOT branch=main.  Upstream cuts release tags off release
# branches, so v1.28.0's commit is NOT an ancestor of main -- GitHub's
# compare API reports main...da9b5e3 as "diverged".  With branch=main the
# fetcher rejects the pin outright:
#   ERROR: onnxruntime-1.28.0-r0 do_fetch: Fetcher failure: Unable to find
#   revision da9b5e364c465de65c49d91e696cd6485270757f in branch main even
#   from upstream
# The SRCREV below is the tag's own commit (verified against the GitHub API,
# tag v1.28.0 -> da9b5e3, 2026-07-25), which is what we actually want pinned;
# nobranch=1 is the standard idiom for a SHA that no tracked branch contains.
#
# PLAIN git://, NOT gitsm://.  da9b5e3's .gitmodules pins exactly three
# submodules: cmake/external/onnx (onnx/onnx), cmake/external/libprotobuf-
# mutator (google/libprotobuf-mutator), and cmake/external/emsdk
# (emscripten-core/emsdk) -- confirmed by `git show da9b5e3:.gitmodules`
# and `git ls-tree -d` against each path. None of the three is consumed by
# this build: onnx/onnx is fetched a second way, by
# cmake/onnxruntime_external_deps.cmake's own onnxruntime_fetchcontent_
# declare(onnx URL ${DEP_URL_onnx} ...) off cmake/deps.txt (the zip staged
# below via FETCHCONTENT_SOURCE_DIR_ONNX) -- confirmed by grepping that
# file at da9b5e3: nothing add_subdirectory()s cmake/external/onnx.
# libprotobuf-mutator is add_subdirectory()'d only from
# cmake/onnxruntime_fuzz_test.cmake, gated on fuzz testing this recipe
# never turns on. emsdk is Emscripten/WASM-only, never this cross-compile's
# target. A gitsm:// fetch would recursively clone all three anyway --
# real bandwidth for content this build discards -- and would leave TWO
# independent pins on onnx/onnx (the submodule commit and the deps.txt
# zip) to keep in sync by hand on every version bump. They happen to
# agree today (submodule pin 2bb50465112feca9003e1ed654d77f01ff1415ca IS
# the v1.22.0 tag onnx/onnx zip below also targets -- verified: `git
# describe --tags 2bb5046...` in the onnx/onnx mirror returns exactly
# v1.22.0), but nothing enforces that agreement going forward. Fetching
# plain git:// (no submodules) makes the zip FetchContent pin the ONLY
# mechanism that ever supplies onnx/onnx, which is also the one this
# recipe's Apache-2.0 LIC_FILES_CHKSUM entry above actually audits.
SRC_URI = "git://github.com/microsoft/onnxruntime.git;protocol=https;nobranch=1"
SRCREV = "da9b5e364c465de65c49d91e696cd6485270757f"
S = "${WORKDIR}/git"

# Carried over from NXP's onnxruntime.inc, which proves these are the real
# link-time deps for a shared-lib ORT build; protobuf-native is ADDED to
# that set (NXP's fork does not need it the same way -- see the
# ONNX_CUSTOM_PROTOC_EXECUTABLE comment below) because our build must not
# let deps.txt's prebuilt protoc_linux_aarch64 binary stand in for a host
# protoc during a cross-compile.
DEPENDS = "zlib libpng protobuf protobuf-native"

inherit cmake python3native

OECMAKE_SOURCEPATH = "${S}/cmake"

# ---------------------------------------------------------------------
# OFFLINE BUILD.  cmake/deps.txt at this tag is a fully pinned CSV
# (Name;Url;SHA1).  Fetched by gh api at this tag on 2026-08-05 (not
# transcribed from NXP or from memory of an older ORT version) -- 39
# entries, not the ~45 the parent plan estimated before anyone read the
# real file at v1.28.0.
#
# NXP's onnxruntime.inc sets -DFETCHCONTENT_FULLY_DISCONNECTED=OFF and lets
# CMake reach the network in do_configure/do_compile to satisfy these.  That
# is a shortcut, not a necessity, and it is a real Yocto offline-build
# violation -- do_compile touching the network fails on an isolated
# builder even though it will pass on any dev box with an open uplink. This
# recipe does not take that shortcut: every dependency this build actually
# needs is pinned as its own SRC_URI entry below (checksummed with the SAME
# sha1sum deps.txt already carries, PLUS the sha256sum BitBake's own
# fetcher additionally requires -- `bitbake onnxruntime` on a real BitBake
# host printed the exact sha256 for each of the 11 entries verbatim; those
# are transcribed below, not re-derived by hand) and pointed at with
# -DFETCHCONTENT_SOURCE_DIR_<NAME>, so FetchContent's own fetch step is a
# no-op: CMake finds the source already sitting in WORKDIR.
#
# -DFETCHCONTENT_FULLY_DISCONNECTED=ON in EXTRA_OECMAKE below is what
# actually enforces "never dials out" -- without it, this whole
# INCLUDED/EXCLUDED accounting only covers the 11 names actually given a
# FETCHCONTENT_SOURCE_DIR_*, and ANY other name CMake's FetchContent asks
# for (a gap in the include/exclude reasoning below, an ORT CMake option
# this recipe didn't anticipate) falls through to a live network fetch
# during do_configure/do_compile -- silently, on a dev box with an open
# uplink, and that is the exact offline-build violation this recipe exists
# to avoid. Setting FETCHCONTENT_FULLY_DISCONNECTED=ON turns that silent
# fetch into a loud CMake configure-time failure that names the missing
# dependency directly, which is the whole point: a fast, informative
# failure beats a build that quietly worked on this host and will not on
# an isolated builder.
#
# THE INDUCED SET IS BEST-EFFORT, NOT EMPIRICAL.  The plan (Task 3) asks
# for the actual set CMake requests, obtained by configuring once and
# reading the FetchContent calls it makes for THIS EXACT EXTRA_OECMAKE
# (BUILD_SHARED_LIB=ON, BUILD_UNIT_TESTS=OFF, ENABLE_PYTHON=OFF,
# USE_XNNPACK unset/default, USE_MIMALLOC unset/default, USE_KLEIDIAI
# unset/default).  That requires a working CMake configure against ORT's
# real cmake/CMakeLists.txt, which this host cannot do (no bitbake, no
# aarch64 cross toolchain, no point downloading the multi-hundred-MB
# submodule tree just to watch what one `cmake` invocation asks for on a
# throwaway x86_64 sysroot that proves nothing about the real cross build).
# So the include/exclude split below is derived from deps.txt's own
# entries plus general knowledge of which ORT CMake option gates each one
# -- it is exactly the kind of claim Step 3 of Task 3 (bitbake -p /
# bitbake-layers show-recipes) and Step 4 (bitbake onnxruntime) exist to
# either confirm or falsify.  If a real configure on the BitBake host
# requests a FetchContent name not listed here, do_configure will attempt
# a network fetch and fail loudly on an isolated builder -- that failure
# names the missing dependency directly; add it here and re-parse. This
# is the single largest unverified claim this recipe file makes.
#
# EXCLUDED, WITH REASONS (every deps.txt entry NOT staged below, and why):
#   coremltools         - CoreML EP (macOS/iOS only), never reachable here
#   directx_headers     - DirectML EP (Windows only)
#   cudnn_frontend       - CUDA EP
#   cutlass              - CUDA EP (GEMM kernels)
#   dawn                 - WebGPU EP
#   onnx_tensorrt         - TensorRT EP (CUDA)
#   vulkan_headers        - Vulkan compute EP
#   tensorboard           - training/profiling visualisation tooling
#   google_benchmark      - perf microbenchmarks, not built (no BUILD_BENCHMARKS)
#   googletest            - unit test framework; matches BUILD_UNIT_TESTS=OFF above
#   pybind11              - Python bindings; matches ENABLE_PYTHON=OFF above
#   cxxopts               - CLI-arg parsing for perf_test/tools binaries this
#                           recipe never builds (no BUILD_UNIT_TESTS, no
#                           perf-test target); LOWEST-CONFIDENCE exclusion in
#                           this list -- flagged, not just asserted
#   microsoft_wil         - Windows Implementation Library, Windows-only
#   extensions (onnxruntime-extensions) - opt-in custom-op library, its own
#                           CMake option, not requested by EXTRA_OECMAKE here
#   mimalloc              - opt-in allocator (-Donnxruntime_USE_MIMALLOC),
#                           default OFF, not set here
#   dlpack                - EMPIRICALLY confirmed dead, not just asserted:
#                           cmake/CMakeLists.txt's cmake_dependent_option
#                           for onnxruntime_ENABLE_DLPACK defaults it ON
#                           only when onnxruntime_ENABLE_TRAINING,
#                           onnxruntime_ENABLE_ATEN, or
#                           onnxruntime_ENABLE_PYTHON is ON -- default OFF
#                           otherwise. This recipe sets none of the three
#                           (ENABLE_PYTHON=OFF explicitly, the other two
#                           untouched at their own OFF default), so
#                           cmake/external/onnxruntime_external_deps.cmake's
#                           `if(onnxruntime_ENABLE_DLPACK)` block that
#                           FetchContent-declares `dlpack` never runs --
#                           confirmed by a real do_configure log listing
#                           FETCHCONTENT_SOURCE_DIR_DLPACK among the
#                           "Manually-specified variables were not used by
#                           the project". Originally miscategorised as
#                           INCLUDED; moved here and dropped from SRC_URI/
#                           EXTRA_OECMAKE once the real configure proved it
#                           unreachable.
#   kleidiai, kleidiai-qmx - Arm Kleidi micro-kernel EP
#                           (-Donnxruntime_USE_KLEIDIAI), default OFF; the
#                           plan names this an explicit follow-up, not this
#                           backend -- not enabled here even though the
#                           dependency happens to already be pinned upstream
#   googlexnnpack, fp16, fxdiv, psimd, pthreadpool - the XNNPACK EP
#                           dependency chain (-Donnxruntime_USE_XNNPACK,
#                           default OFF); this recipe requests the CPU EP's
#                           own MLAS kernels only, not an XNNPACK EP
#   protoc_win64, protoc_win32, protoc_linux_x86, protoc_mac_universal -
#                           prebuilt protoc binaries for platforms that are
#                           never this cross-compile's build OR host arch
#
#   protoc_linux_aarch64  - THE NAMED TRAP. This is a PREBUILT protoc
#                           binary for the TARGET (aarch64) arch. A
#                           cross-compiling BitBake build host is x86_64;
#                           it cannot execute an aarch64 protoc, and even if
#                           it somehow could, running a downloaded prebuilt
#                           during do_compile is exactly the offline-model
#                           violation this recipe exists to avoid. OE's
#                           protobuf-native (already in DEPENDS above)
#                           supplies a HOST-executable protoc instead.
#                           ONNX's CMake (which ORT's cmake/CMakeLists.txt
#                           calls into for the onnx FetchContent dependency)
#                           exposes ONNX_CUSTOM_PROTOC_EXECUTABLE precisely for this
#                           cross-compile case -- pointed at
#                           protobuf-native's staged protoc below. NOTE:
#                           this exact variable name is carried from
#                           general ONNX/ORT cross-compile knowledge, not
#                           confirmed against v1.28.0's cmake/CMakeLists.txt
#                           text -- another line item for Task 3 Step 3's
#                           parse-check to prove or correct.
#
# INCLUDED (best-effort "core, not gated by an option we leave off" set;
# each FETCHCONTENT_SOURCE_DIR_<NAME> below points one level INTO the
# archive, because a GitHub tag/commit archive zip's own top-level folder
# is "{reponame}-{ref}" (ref with a leading "v" stripped for vX.Y.Z-style
# tags) -- inferred from GitHub's own well-documented archive-naming
# convention, NOT verified by actually unzipping any of these 11 archives
# on this host):
SRC_URI += " \
    https://github.com/abseil/abseil-cpp/archive/refs/tags/20250814.0.zip;name=abseil_cpp;subdir=deps/abseil_cpp;sha1sum=a9eb1d648cbca4d4d788737e971a6a7a63726b07 \
    https://github.com/HowardHinnant/date/archive/refs/tags/v3.0.1.zip;name=date;subdir=deps/date;sha1sum=2dac0c81dc54ebdd8f8d073a75c053b04b56e159 \
    https://github.com/eigen-mirror/eigen/archive/1d8b82b0740839c0de7f1242a3585e3390ff5f33/eigen-1d8b82b0740839c0de7f1242a3585e3390ff5f33.zip;name=eigen;subdir=deps/eigen;sha1sum=05b19b49e6fbb91246be711d801160528c135e34 \
    https://github.com/google/flatbuffers/archive/refs/tags/v23.5.26.zip;name=flatbuffers;subdir=deps/flatbuffers;sha1sum=59422c3b5e573dd192fead2834d25951f1c1670c \
    https://github.com/nlohmann/json/archive/refs/tags/v3.11.3.zip;name=json;subdir=deps/json;sha1sum=5e88795165cc8590138d1f47ce94ee567b85b4d6 \
    https://github.com/microsoft/GSL/archive/refs/tags/v4.2.1.zip;name=microsoft_gsl;subdir=deps/microsoft_gsl;sha1sum=1094e3bb7a8af763dcb136ccd676e6e75e614eec \
    https://github.com/boostorg/mp11/archive/refs/tags/boost-1.82.0.zip;name=mp11;subdir=deps/mp11;sha1sum=9bc9e01dffb64d9e0773b2e44d2f22c51aace063 \
    https://github.com/onnx/onnx/archive/refs/tags/v1.22.0.zip;name=onnx;subdir=deps/onnx;sha1sum=2b2cd58ac7a26df5371266149e0c76776330cdf1 \
    https://github.com/pytorch/cpuinfo/archive/4628dc060ce4e82345dc166bbac875609db4ff69.zip;name=pytorch_cpuinfo;subdir=deps/pytorch_cpuinfo;sha1sum=e58d4b47c16a982111c897e669ae4f1821a393d7 \
    https://github.com/google/re2/archive/refs/tags/2024-07-02.zip;name=re2;subdir=deps/re2;sha1sum=646e1728269cde7fcef990bf4a8e87b047882e88 \
    https://github.com/dcleblanc/SafeInt/archive/refs/tags/3.0.28.zip;name=safeint;subdir=deps/safeint;sha1sum=23f252040ff6cb9f1fd18575b32fa8fb5928daac \
"

# BitBake's own fetcher requires a sha256sum in addition to the sha1sum
# inline on each SRC_URI entry above -- sha1sum alone is not enough for
# BitBake, even though it is what cmake/deps.txt itself carries and is
# what the FETCHCONTENT_SOURCE_DIR_* / INCLUDED-vs-EXCLUDED reasoning
# above cross-checks against upstream. These are NOT re-derived by hand;
# they are BitBake's own `do_fetch` "Missing SRC_URI checksum" error output
# on a real BitBake host, transcribed verbatim:
SRC_URI[abseil_cpp.sha256sum] = "b2bdcf6682d8cb53df365bcc5d6c318a22e55821d9978a10fdb61404c026daff"
SRC_URI[date.sha256sum] = "f4300b96f7a304d4ef9bf6e0fa3ded72159f7f2d0f605bdde3e030a0dba7cf9f"
SRC_URI[eigen.sha256sum] = "6a60d76351f97132669daeeb721d6bf14b008101883ad2d687a3201c5c461eb0"
SRC_URI[flatbuffers.sha256sum] = "57bd580c0772fd1a726c34ab8bf05325293bc5f9c165060a898afa1feeeb95e1"
SRC_URI[json.sha256sum] = "04022b05d806eb5ff73023c280b68697d12b93e1b7267a0b22a1a39ec7578069"
SRC_URI[microsoft_gsl.sha256sum] = "c9291d95f5f6e5c561990d06a589b01d89e553d0f366f0ce723dd1788e7a6076"
SRC_URI[mp11.sha256sum] = "81431bdc44c439a324e02c07ed067f8f556419fd86f2d8b486ff568df6aac899"
SRC_URI[onnx.sha256sum] = "8dc1181d33529a1249e031226126d0699ac9bdfc571ee530ee3a12f4656f2be3"
SRC_URI[pytorch_cpuinfo.sha256sum] = "2ed3ebc6c2656cc0aafc7af319e5cb0f97cc9b415eae180f566def84f1ca6a29"
SRC_URI[re2.sha256sum] = "a835fe55fbdcd8e80f38584ab22d0840662c67f2feb36bd679402da9641dc71e"
SRC_URI[safeint.sha256sum] = "3ffbd9a2fdff45da77da3e7269e9aa512ea43bed5c38ce8fd8f3d1068a032c3f"

# FETCHCONTENT_SOURCE_DIR_<NAME> must match the name CMake's own
# FetchContent_Declare() call used, upper-cased -- NOT the deps.txt/SRC_URI
# name, which is just this recipe's own bitbake-fetcher label and is free
# to differ. A real do_configure proved three of the twelve didn't match:
# cmake/external/onnxruntime_external_deps.cmake declares json's dependency
# as `nlohmann_json` (-> ...SOURCE_DIR_NLOHMANN_JSON) and GSL's as `GSL`
# (-> ...SOURCE_DIR_GSL), and cmake/external/eigen.cmake declares eigen's
# as `Eigen3` (-> ...SOURCE_DIR_EIGEN3) -- confirmed by grepping
# onnxruntime_fetchcontent_declare(...) in the real unpacked tree, not
# guessed. With the old (wrong) names, CMake silently ignored our staged
# source, fell back to its FIND_PACKAGE_ARGS-driven find_package() (which
# fails outright under FETCHCONTENT_FULLY_DISCONNECTED=ON), and
# do_configure died on "target ... was not found" for onnxruntime_common
# and onnxruntime. meta-oe ships nlohmann-json at the exact same 3.11.3
# this recipe pins -- a legitimate DEPENDS swap for that one -- but
# libeigen (3.4.0) and microsoft-gsl (4.0.0) in meta-oe do NOT match this
# recipe's deps.txt pins (a post-3.4 eigen commit, GSL v4.2.1), so they are
# not safe substitutes; keeping all three as staged FetchContent sources
# keeps one consistent mechanism instead of splitting it dependency-by-
# dependency for an inexact win.
# THE REAL INJECTOR, FOUND BY READING build.ninja BACKWARDS FROM THE FAILING
# OBJECT FILE'S OWN FLAGS LINE -- not by grepping for a literal "-Werror"
# string, which is why the first pass over cmake/CMakeLists.txt's
# ORT_WARNING_FLAGS list and every deps/*/CMakeLists.txt missed it.
#
# build/build.ninja's FLAGS line for tensorprotoutils.cc.o (part of the
# onnxruntime_framework target) ends "... -Wall -Wextra -Wno-deprecated-copy
# -Wno-nonnull-compare -Wno-interference-size -Werror" -- a BARE -Werror as
# the very LAST token, after every target_compile_options() entry ORT's own
# onnxruntime_set_compile_flags() adds. No target_compile_options call
# anywhere in cmake/*.cmake or any vendored dep's CMakeLists.txt passes a
# literal "-Werror" string (checked again, still true) -- it is not a
# compile OPTION at all. It is CMake's own COMPILE_WARNING_AS_ERROR TARGET
# PROPERTY, synthesised into a trailing -Werror by the Ninja generator
# itself, which is exactly why no source grep for "-Werror" ever finds it.
# The setter is cmake/CMakeLists.txt:1119, inside the same
# onnxruntime_set_compile_flags() function called for every ORT-owned
# target (onnxruntime_framework included):
#   set_target_properties(${target_name} PROPERTIES COMPILE_WARNING_AS_ERROR ON)
# unconditional -- no onnxruntime_USE_CUDA / compiler-ID / version guard.
# (cmake/external/helper_functions.cmake turns the SAME property back OFF,
# but only for the vendored deps' OWN subdirectory targets, e.g. absl_* --
# that cannot help here: inlined_vector.h's false positive fires while
# compiling ORT's OWN tensorprotoutils.cc, which merely #includes the
# abseil header; the flags in force are onnxruntime_framework's, not
# abseil's.) Reproducible at ninja step [535/983]:
#   deps/abseil_cpp/abseil-cpp-20250814.0/absl/container/inlined_vector.h:684:29:
#   error: 'indices_values' may be used uninitialized [-Werror=maybe-uninitialized]
#
# WHY CXXFLAGS:append = " -Wno-error" (this recipe's first attempt at a
# fix, now REMOVED -- see below) did nothing: it lands in CMAKE_CXX_FLAGS,
# which CMake's Ninja generator emits FIRST on the compile line (confirmed
# in this same FLAGS output: "...-fvisibility-inlines-hidden -Wno-error
# -ffunction-sections..." is CMAKE_CXX_FLAGS's own tail, transcribed
# verbatim from CMakeCache.txt). The COMPILE_WARNING_AS_ERROR property's
# -Werror is appended by the generator AFTER every target_compile_options()
# entry -- provably the last token on the line. GCC resolves a repeated
# warning-error class by LAST occurrence on the command line, so a
# -Wno-error near the front loses to a bare -Werror at the back. Confirmed
# by reading build.ninja, not assumed -- do not re-add CXXFLAGS:append
# -Wno-error; it is a dead flag on this CMake/ninja pairing.
#
# THE FIX: CMake exposes exactly one first-class escape hatch for this --
# a *configure-time command-line flag*, not a -D cache variable, that
# overrides the property (and CMAKE_COMPILE_WARNING_AS_ERROR) everywhere,
# for every target, no matter how or where set_target_properties() set it:
#   --compile-no-warning-as-error
# `cmake --help` on this host's CMake 3.28.3: "Ignore
# COMPILE_WARNING_AS_ERROR property and CMAKE_COMPILE_WARNING_AS_ERROR
# variable."; the property's own docs: "If the cmake
# --compile-no-warning-as-error option is given on the cmake(1) command
# line, this property is ignored." -- added in CMake 3.24 alongside the
# property itself, so any Yocto cmake-native recent enough to honour the
# property honours the override flag too. It is passed below as a bare
# EXTRA_OECMAKE token, not a `-D<VAR>=` entry, because cmake.bbclass's
# cmake_do_configure() appends EXTRA_OECMAKE's contents literally onto the
# `cmake -S ... -B ...` command line (see EXTRA_OECMAKE in
# meta/classes-recipe/cmake.bbclass) -- this is a CLI switch, not a cache
# variable, so `-D--compile-no-warning-as-error` would be wrong.
#
# This is preferred over -Wno-error precisely because it defeats the
# PROPERTY at its own mechanism, not a flag-order race: it does not depend
# on where any given CMake version's generator happens to place tokens,
# and it leaves Yocto's own -Werror=format-security hardening (an
# unrelated, explicit CXXFLAGS entry, not the COMPILE_WARNING_AS_ERROR
# property) and CMAKE_CXX_FLAGS untouched.
#
# The position, stated plainly: we are PACKAGING a tagged upstream release,
# not developing ORT. Upstream's warning-as-error policy is calibrated to
# upstream's own CI compiler matrix and is not our quality gate; our
# quality gate is alp-sdk's own code. Do NOT narrow this to un-setting
# COMPILE_WARNING_AS_ERROR on just onnxruntime_framework, or to
# -Wno-error=maybe-uninitialized on just this one diagnostic -- the same
# class of GCC/abseil false positive can recur in any other ORT target
# that instantiates the same absl::InlinedVector specialization, and the
# next GCC bump can trip a different -Werror=<diagnostic> false positive
# in any of the vendored deps.

# BUILDPATHS LEAK, found by a real do_package_qa WARNING (not fatal, but
# not ignorable -- an artifact we redistribute should not embed this
# build's own host tmp path):
#   WARNING: onnxruntime-1.28.0-r0 do_package_qa: QA Issue: File
#   /usr/lib/libonnxruntime.so.1.28.0 in package onnxruntime contains
#   reference to TMPDIR [buildpaths]
# ROOT CAUSE: onnx/onnx's ONNX_OPERATOR_SET_SCHEMA macro
# (onnx/defs/schema.h:1335, :1358) captures __FILE__ into every operator
# schema registration call, so the compiler's macro-expanded source path
# for every onnx/onnx translation unit ends up as a real string in
# libonnxruntime.so's own .rodata -- not just its DWARF debug info, which
# is why the WARNING is on the shipped "onnxruntime" package itself, not
# only "onnxruntime-dbg". Yocto's own bitbake.conf DEBUG_PREFIX_MAP only
# rewrites __FILE__/debug-info paths under ${S}, ${B}, and the sysroots
# (`-fmacro-prefix-map=${S}=... -fmacro-prefix-map=${B}=...`, etc; see
# meta/conf/bitbake.conf). It has no entry for ${WORKDIR}/deps -- because
# ${WORKDIR}/deps is where THIS recipe's own offline-FetchContent-staging
# design (see the OFFLINE BUILD comment above) puts onnx/onnx and the
# other 10 dependencies, entirely outside ${S} (=${WORKDIR}/git) and ${B}
# (=${WORKDIR}/build). It is not an ORT or onnx defect and needs no
# upstream patch: it is a gap in this recipe's own prefix-map coverage,
# introduced by this recipe's own choice of staging directory, closed by
# extending the same prefix-map mechanism Yocto already uses for ${S}/${B}
# to also cover ${WORKDIR}/deps. dlopen()able correctness, not just
# reproducibility, is why this earns a real fix rather than an accepted
# deviation: an embedded TMPDIR path from *this developer's own machine*
# in a redistributed .so is exactly the kind of accidental host leak
# LIC_FILES_CHKSUM-adjacent hygiene exists to catch.
CXXFLAGS:append = " \
    -fmacro-prefix-map=${WORKDIR}/deps=${TARGET_DBGSRC_DIR}/deps \
    -fdebug-prefix-map=${WORKDIR}/deps=${TARGET_DBGSRC_DIR}/deps \
"

EXTRA_OECMAKE += " \
    --compile-no-warning-as-error \
    -Donnxruntime_BUILD_SHARED_LIB=ON \
    -Donnxruntime_BUILD_UNIT_TESTS=OFF \
    -Donnxruntime_ENABLE_PYTHON=OFF \
    -DCMAKE_BUILD_TYPE=Release \
    -DONNX_CUSTOM_PROTOC_EXECUTABLE=${STAGING_BINDIR_NATIVE}/protoc \
    -DFETCHCONTENT_FULLY_DISCONNECTED=ON \
    -DFETCHCONTENT_SOURCE_DIR_ABSEIL_CPP=${WORKDIR}/deps/abseil_cpp/abseil-cpp-20250814.0 \
    -DFETCHCONTENT_SOURCE_DIR_DATE=${WORKDIR}/deps/date/date-3.0.1 \
    -DFETCHCONTENT_SOURCE_DIR_EIGEN3=${WORKDIR}/deps/eigen/eigen-1d8b82b0740839c0de7f1242a3585e3390ff5f33 \
    -DFETCHCONTENT_SOURCE_DIR_FLATBUFFERS=${WORKDIR}/deps/flatbuffers/flatbuffers-23.5.26 \
    -DFETCHCONTENT_SOURCE_DIR_NLOHMANN_JSON=${WORKDIR}/deps/json/json-3.11.3 \
    -DFETCHCONTENT_SOURCE_DIR_GSL=${WORKDIR}/deps/microsoft_gsl/GSL-4.2.1 \
    -DFETCHCONTENT_SOURCE_DIR_MP11=${WORKDIR}/deps/mp11/mp11-boost-1.82.0 \
    -DFETCHCONTENT_SOURCE_DIR_ONNX=${WORKDIR}/deps/onnx/onnx-1.22.0 \
    -DFETCHCONTENT_SOURCE_DIR_PYTORCH_CPUINFO=${WORKDIR}/deps/pytorch_cpuinfo/cpuinfo-4628dc060ce4e82345dc166bbac875609db4ff69 \
    -DFETCHCONTENT_SOURCE_DIR_RE2=${WORKDIR}/deps/re2/re2-2024-07-02 \
    -DFETCHCONTENT_SOURCE_DIR_SAFEINT=${WORKDIR}/deps/safeint/SafeInt-3.0.28 \
"

# `onnxruntime_BUILD_UNIT_TESTS=OFF` and `ENABLE_PYTHON=OFF` diverge from
# NXP's onnxruntime.inc deliberately: we ship a runtime, not a test suite or
# Python bindings, and pulling either back in reopens the googletest /
# google_benchmark / pybind11 dependency trees this recipe deliberately
# excludes above.

# libonnxruntime_providers_shared.so NEEDS EXPLICIT REPACKAGING, found by a
# real do_package_qa failure (dev-elf), not anticipated up front:
#   ERROR: onnxruntime-1.28.0-r0 do_package_qa: QA Issue: -dev package
#   onnxruntime-dev contains non-symlink .so
#   '/usr/lib/libonnxruntime_providers_shared.so' [dev-elf]
# `readelf -d` confirms it is its own real ELF (SONAME
# libonnxruntime_providers_shared.so, no versioned companion, static-pie),
# and `readelf -d libonnxruntime.so.*` shows it is NOT in libonnxruntime.so's
# own NEEDED list -- it is ORT's provider-plugin ABI shim, loaded by
# execution providers via dlopen() at runtime by name, not linked against
# at build time by anything this recipe builds (CPU EP only; no
# CUDA/TensorRT/OpenVINO provider .so is built here to dlopen it yet, but
# upstream ships this shim unconditionally as part of the runtime, and a
# future EP enabled from a customer's board.yaml would dlopen it by this
# exact path).
#
# THE FIRST VERSION OF THIS FIX (a `FILES:${PN}-dev:remove =
# "${libdir}/libonnxruntime_providers_shared.so"` paired with `FILES:${PN}
# += "${libdir}/libonnxruntime_providers_shared.so"`) DID NOT WORK --
# re-running the real build and re-checking packages-split/ (not just
# trusting a green do_package_qa) proved the file was still landing in
# onnxruntime-dev, unchanged. Two independent reasons, both had to be
# fixed, not one:
#   1. `:remove` deletes an exact literal whitespace-delimited token from a
#      variable's value; it cannot subtract one path out of a GLOB that is
#      still present and still matches it. bitbake.conf's default
#      FILES:${PN}-dev pulls in FILES_SOLIBSDEV = "${base_libdir}/lib*.so
#      ${libdir}/lib*.so" -- base_libdir == libdir on this non-multilib
#      target, so in practice it is the one glob "${libdir}/lib*.so"
#      appearing twice. The literal string
#      "${libdir}/libonnxruntime_providers_shared.so" was never actually a
#      token in FILES:${PN}-dev's value to begin with -- the file was only
#      ever caught by the glob, never named -- so ":remove"ing that literal
#      string removed nothing, and the untouched glob kept matching the
#      file at package-split time exactly as before.
#   2. PACKAGES orders ${PN}-dev strictly before ${PN} (bitbake.conf:
#      "${PN}-src ${PN}-dbg ${PN}-staticdev ${PN}-dev ${PN}-doc
#      ${PN}-locale ${PACKAGE_BEFORE_PN} ${PN}"), and package-splitting
#      assigns each file to the FIRST package in that order whose FILES
#      matches it. ${PN} is dead last, so even a fully correct FILES:${PN}
#      claim can never win a file ${PN}-dev's glob already matched.
#
# THE ACTUAL FIX: stop the glob from ever seeing this file at all, by
# clearing FILES_SOLIBSDEV outright and naming the one REAL dev symlink
# this recipe actually produces (libonnxruntime.so -> libonnxruntime.so.1
# -> libonnxruntime.so.1.28.0) explicitly instead, so -dev still gets
# exactly the one link it is supposed to. `find packages-split/` after
# rebuilding with this fix confirmed the file's package by location, not
# by QA silence alone:
#   packages-split/onnxruntime/usr/lib/libonnxruntime_providers_shared.so
#   packages-split/onnxruntime-dev/usr/lib/libonnxruntime.so   (still a symlink)
# Checked for a second bare/unversioned .so the same glob might have swept
# up before trusting this was the whole fix: there is only ever the one --
# `find image/ -iname '*.so' -o -iname '*.so.*'` on a real build lists
# exactly libonnxruntime.so (symlink), libonnxruntime.so.1 (symlink),
# libonnxruntime.so.1.28.0 (real), and libonnxruntime_providers_shared.so
# (real) -- nothing else bare.
#
# Do NOT fix this by adding `dev-elf` to INSANE_SKIP instead: that would
# silence the QA check but leave the plugin shim living only in
# onnxruntime-dev, so any production image that installs the runtime
# "onnxruntime" package without "onnxruntime-dev" (the normal shape of a
# deployed image) would ship without it -- a silent runtime gap for the
# first execution-provider plugin that ever dlopen()s it.
FILES_SOLIBSDEV = ""
FILES:${PN} += "${libdir}/libonnxruntime_providers_shared.so"
FILES:${PN}-dev += "${libdir}/libonnxruntime.so"

# ---------------------------------------------------------------------
# VERIFICATION STATUS, STATED PLAINLY, NOT OVERSTATED:
#
#   - PARSED: yes. `bitbake -p` on a real BitBake host with meta-alp-sdk
#     layered in succeeded (5222 .bb files, 0 errors), and
#     `bitbake-layers show-recipes onnxruntime` resolves this recipe:
#     "onnxruntime: meta-alp-ort-scratch 1.28.0".
#   - do_fetch / do_configure / do_compile / do_install: ALL GREEN on a real
#     BitBake host (build-ort, MACHINE e1m-v2n101-a55, DISTRO alp), transcribed
#     from an actual run's log, not inferred:
#       do_compile:  ninja [983/983], zero `error:` lines.
#       do_install:  Succeeded.
#     Artifacts genuinely present on disk afterwards
#     (tmp/work/cortexa55-poky-linux/onnxruntime/1.28.0/image/):
#       usr/lib/libonnxruntime.so -> libonnxruntime.so.1 -> libonnxruntime.so.1.28.0
#       usr/lib/libonnxruntime_providers_shared.so  (real ELF, no version)
#       usr/include/onnxruntime/onnxruntime_c_api.h
#     The include path is a subdirectory (usr/include/onnxruntime/), NOT a
#     flat usr/include/onnxruntime_c_api.h -- src/yocto/CMakeLists.txt's
#     find_path(ORT_INCLUDE_DIR onnxruntime_c_api.h) needed a matching
#     PATH_SUFFIXES entry or it would silently resolve to a backend-less
#     build; fixed separately in that file (not touched here).
#   - The FETCHCONTENT_SOURCE_DIR_* include set, the nested "{reponame}-
#     {ref}" unpack path, and ONNX_CUSTOM_PROTOC_EXECUTABLE as the correct
#     cross-protoc variable name are no longer inferred -- do_configure and
#     do_compile going green on the actual FETCHCONTENT_FULLY_DISCONNECTED=ON
#     build IS the confirmation Task 3 asked for.
#   - do_package_qa: found two further real, previously-unobservable defects
#     (only reachable once do_compile/do_install actually ran) -- see the
#     "NEEDS EXPLICIT REPACKAGING" [dev-elf] comment and the "BUILDPATHS
#     LEAK" [buildpaths] comment above, both fixed in this same pass.
#     do_package_qa itself has NOT yet been re-run against those two fixes
#     -- that is the one remaining thing between this recipe and a fully
#     green `bitbake onnxruntime` (do_package / do_populate_sysroot / the
#     rest of the task graph were already green before these two fixes and
#     are not expected to regress).
#
# Re-run, and only with the maintainer's separate go-ahead for the long
# build (full history clone, significant disk):
#
#     bitbake -p
#     bitbake-layers show-recipes onnxruntime
#     bitbake onnxruntime
#
# A failure at do_package_qa naming a *different* file than the two fixed
# above means a third bare/unversioned .so or a third unmapped source root
# exists; do not paper over either class by reaching for INSANE_SKIP or by
# flipping FETCHCONTENT_FULLY_DISCONNECTED to OFF -- the latter is the
# exact offline-build violation this recipe exists to rule out.
