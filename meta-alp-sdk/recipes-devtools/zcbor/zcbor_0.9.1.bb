# SPDX-License-Identifier: Apache-2.0
#
# zcbor -- CBOR (RFC 8949) codec for the Cortex-A55 Linux side.
#
# WHY THIS EXISTS. src/common/alp_model.c (the real .alpmodel manifest
# parser behind <alp/model.h>) decodes the container via zcbor's
# zcbor_decode.h.  Until this recipe, zcbor was a Zephyr-only west module
# (metadata/libraries/zcbor.yaml carried only an `integration.zephyr:`
# key) -- no plain-CMake (baremetal/Yocto) build staged its headers or
# library, so src/yocto/CMakeLists.txt compiled the NOSUPPORT stub
# (src/common/stub/stub_model.c) instead, unconditionally, and always
# will until this dependency exists on the sysroot (issue #1254).  This
# recipe is that dependency.
#
# NO UPSTREAM BUILD SYSTEM.  Unlike onnxruntime (this layer's other
# devtools recipe), zcbor ships NO CMakeLists.txt / Meson / Makefile
# anywhere in its own tree -- confirmed by walking a real checkout at the
# pin below (`git ls-tree -r --name-only`): the only CMakeLists.txt files
# under the whole repo live in samples/ and tests/, throwaway per-sample
# and per-test fixtures, not a library build.  The Zephyr west module
# hits the exact same gap and is declared to work around it the same way:
# zephyr/module.yml sets `build: cmake-ext: true`, which tells west "the
# CONSUMER supplies the CMakeLists, not this module" -- the actual glue
# that compiles zcbor's four .c files lives OUTSIDE the module entirely,
# at $ZEPHYR_BASE/modules/zcbor/CMakeLists.txt (verified against a real
# west workspace: $ZEPHYR_BASE/modules/zcbor/CMakeLists.txt names
# exactly src/zcbor_common.c, src/zcbor_decode.c, src/zcbor_encode.c and
# src/zcbor_print.c).  do_compile below is the OE-side equivalent of that
# external glue, compiling the SAME four translation units -- not a
# rediscovery, a transcription of what the Zephyr side already proves
# is the correct source list.
#
# UPSTREAM + PIN.  west.yml pins the `zcbor` module to commit
# 9164bd18dcd88ff9d9ef98279501fc1093571017 on the zephyrproject-rtos
# mirror -- a mutable branch tip, not a tag on either remote (confirmed:
# no tag on the mirror or on the canonical NordicSemiconductor/zcbor
# resolves to it, and the canonical repo's `main` has since been
# rewritten past it, orphaning that SHA there even though the GitHub API
# can still resolve the bare object by SHA -- a plain `git clone` cannot).
# Building against a mirror-only mutable tip has no fallback if that
# mirror ever rewinds `main`: every customer do_fetch would fail with
# nothing to retry against.
#
# This recipe instead pins the canonical remote at a real tag:
#   tag 0.9.1 = 9b07780aca6fb21f82a241ba386ad9b379809337
# verified present on BOTH github.com/NordicSemiconductor/zcbor and
# github.com/zephyrproject-rtos/zcbor.  The two commits between the
# west-pinned SHA and this tag touch only CI workflow + test files, not
# a single line of src/*.c or include/*.h -- confirmed by walking a real
# checkout: the subtrees this recipe actually consumes are byte-identical
# at both revisions (`git rev-parse <rev>:src` /
# `git rev-parse <rev>:include` / `git rev-parse <rev>:LICENSE`):
#   src     = fcada23c96348924b1a4c3040ccce3fa2e89bb29
#   include = 3ae604e5f231711451f13a3ca063c7d7380acdd2
#   LICENSE = d645695673349e3947e8e5ae42332d0ac3164cd7
# So alp_model.c's wire format decodes identically whichever core reads a
# given .alpmodel container -- an M-class Zephyr slice pinned to the
# west SHA today, the A55 Yocto slice here pinned to the tag -- while this
# recipe fetches from the project's own canonical remote, tag-anchored
# rather than tied to a mirror's mutable branch tip. Re-verify this tree
# identity before ever bumping this pin.
#
# ADR 0018 non-goal: no vendors/zcbor/ tree.  do_fetch below pulls
# upstream at build time; this recipe carries no source payload of its
# own, matching the onnxruntime recipe's own no-vendoring stance.
#
# SCOPE.  zcbor.py (the CDDL-schema code generator) is NOT part of this
# recipe's output.  alp_model.c hand-decodes a fixed, versioned container
# format directly against zcbor's low-level zcbor_decode.h primitives
# (zcbor_map_start_decode / zcbor_tstr_decode / ... -- see that file); it
# does not consume any zcbor.py-GENERATED code, so there is no do_compile
# step here that runs the generator, and no python3 DEPENDS to support
# one.  Only the C codec (src/*.c + include/*.h) ships.
DESCRIPTION = "zcbor -- low-footprint CBOR (RFC 8949) encoder/decoder library"
HOMEPAGE = "https://github.com/NordicSemiconductor/zcbor"
LICENSE = "Apache-2.0"
# md5 computed for real off the actual pinned tree's LICENSE file
# (`md5sum LICENSE` against the checkout at the SRCREV below), not
# guessed or carried over from another package -- same standard the
# onnxruntime recipe's compound LIC_FILES_CHKSUM holds itself to.
LIC_FILES_CHKSUM = "file://LICENSE;md5=3b83ef96387f14655fc854ddc3c6bd57"

# branch=main, NOT tag=0.9.1: bitbake's git fetcher refuses a URL revision
# and an SRCREV together -- `Conflicting revisions (... from SRCREV and 0.9.1
# from the url)` -- and it refuses at PARSE time, so the whole layer stops
# loading and every image target fails at once, not just this recipe.
#
# Nothing is unpinned by that.  `branch=` says where to look and is not a
# revision, which is why every sibling recipe here pairs one with an SRCREV.
# The immutable pin is the SRCREV below, and it is exactly what the tag
# resolves to -- `git ls-remote --tags` returns
# 9b07780aca6fb21f82a241ba386ad9b379809337 for refs/tags/0.9.1, with no `^{}`
# deref entry, so 0.9.1 is a lightweight tag on that very commit.
#
# `main` rather than nobranch=1 because the commit genuinely is on it
# (`compare/9b07780...main` -> behind_by: 0), which keeps the fetcher's
# reachability check doing real work instead of switching it off.
#
# The second SRC_URI entry is alp-sdk's own zcbor_abi_pin.h (issue #1254
# follow-up), staged alongside the upstream headers by do_install below --
# see that header's own comment for what it pins and why.
SRC_URI = "git://github.com/NordicSemiconductor/zcbor.git;protocol=https;branch=main \
           file://zcbor_abi_pin.h"
SRCREV = "9b07780aca6fb21f82a241ba386ad9b379809337"
S = "${WORKDIR}/git"
B = "${WORKDIR}/build"

# No `inherit cmake` (or any build-system inherit): there is nothing for
# one to drive, per the header comment above.  do_configure stays the
# empty default from base.bbclass.

# ZCBOR_* ABI-pin (issue #1254 follow-up): deliberately EMPTY, not a
# leftover omission -- see files/zcbor_abi_pin.h for the full reasoning.
# Keep this in lockstep with that header (and with
# metadata/libraries/zcbor.yaml's own record of the same set): do not
# add a -DZCBOR_* flag here without updating both.
ZCBOR_ABI_CFLAGS = ""

do_compile() {
    install -d ${B}
    for src in zcbor_common zcbor_decode zcbor_encode zcbor_print; do
        ${CC} ${CFLAGS} ${CPPFLAGS} ${ZCBOR_ABI_CFLAGS} -I${S}/include -fPIC \
            -c ${S}/src/${src}.c -o ${B}/${src}.o
    done
    ${CC} ${LDFLAGS} -shared -Wl,-soname,libzcbor.so.0 \
        -o ${B}/libzcbor.so.${PV} \
        ${B}/zcbor_common.o ${B}/zcbor_decode.o ${B}/zcbor_encode.o ${B}/zcbor_print.o
}

do_install() {
    install -d ${D}${includedir}
    install -m 0644 ${S}/include/zcbor_common.h ${S}/include/zcbor_decode.h \
        ${S}/include/zcbor_encode.h ${S}/include/zcbor_print.h \
        ${S}/include/zcbor_tags.h ${WORKDIR}/zcbor_abi_pin.h ${D}${includedir}/

    install -d ${D}${libdir}
    install -m 0755 ${B}/libzcbor.so.${PV} ${D}${libdir}/
    ln -sf libzcbor.so.${PV} ${D}${libdir}/libzcbor.so.0
    ln -sf libzcbor.so.0 ${D}${libdir}/libzcbor.so
}

# Explicit, not left to the FILES_SOLIBS/FILES_SOLIBSDEV globs -- this
# recipe only ever produces the one library, so naming its three files
# outright is clearer than trusting a glob default to land them the same
# way onnxruntime_1.28.0.bb had to fight that glob to correct (see that
# recipe's own FILES_SOLIBSDEV comment for the failure mode being
# avoided here by not relying on it at all).
FILES:${PN} = "${libdir}/libzcbor.so.${PV} ${libdir}/libzcbor.so.0"
FILES:${PN}-dev += "${includedir}/zcbor_*.h ${libdir}/libzcbor.so"
