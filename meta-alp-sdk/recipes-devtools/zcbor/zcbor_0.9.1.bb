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
# west workspace: /home/caner/zephyr/modules/zcbor/CMakeLists.txt names
# exactly src/zcbor_common.c, src/zcbor_decode.c, src/zcbor_encode.c and
# src/zcbor_print.c).  do_compile below is the OE-side equivalent of that
# external glue, compiling the SAME four translation units -- not a
# rediscovery, a transcription of what the Zephyr side already proves
# is the correct source list.
#
# UPSTREAM + PIN.  Pinned to the EXACT commit Zephyr's own west.yml pins
# for the `zcbor` module TODAY, not the nearest release tag: revision
# 9164bd18dcd88ff9d9ef98279501fc1093571017 (confirmed against a real
# checkout at /home/caner/modules/lib/zcbor: `git log -1 --format=%H`
# matches, `git describe --tags` reports 0.9.1-2-g9164bd1 -- two commits
# PAST the 0.9.1 tag, not the tag itself).  metadata/libraries/zcbor.yaml's
# `version:` field records the same "0.9.1" human string for the same
# reason ORT's manifest records a release string rather than a bare SHA,
# but the SRCREV actually built here is the exact west-pinned commit,
# deliberately not v0.9.1 itself: alp_model.c's wire format must decode
# identically whichever core reads a given .alpmodel container -- an
# M-class Zephyr slice today, an A55 Yocto slice after this recipe -- and
# pinning the tag here would build a tree two commits removed from the one
# every Zephyr build in this repo actually links. Bump this SRCREV only in
# lockstep with west.yml's own zcbor revision (and re-check
# metadata/libraries/zcbor.yaml's grounding note in the same change);
# never let the two pins drift apart.
#
# THE REMOTE IS NOT THE CANONICAL UPSTREAM -- THIS WAS EMPIRICALLY
# REQUIRED, NOT A STYLE CHOICE.  west.yml's zcbor entry carries no
# `remote:` override, so it resolves through the manifest's own default
# remote (`url-base: https://github.com/zephyrproject-rtos`), i.e. the
# real fetch source west uses is https://github.com/zephyrproject-rtos/
# zcbor.git, a Zephyr-project-maintained mirror -- NOT
# https://github.com/NordicSemiconductor/zcbor.git, the project's own
# canonical repo.  This is not documentation trivia: it is load-bearing.
# A first attempt at this recipe pointed SRC_URI at
# NordicSemiconductor/zcbor.git directly and its do_fetch failed --
# `git clone --bare --mirror` against that repo succeeds (492 refs,
# 9242 objects) but the pinned commit is UNREACHABLE from every one of
# them (`git rev-list --all | grep 9164bd1` -> no match); `git ls-remote`
# against it shows tag 0.9.1 pointing at a DIFFERENT commit
# (9b07780aca6fb21f82a241ba386ad9b379809337) than the one west.yml pins.
# The canonical repo's `main` has since been rewritten past this commit
# (`git compare main...9164bd1` on the GitHub API reports "diverged", 2
# ahead / 142 behind), orphaning it there even though the GitHub API can
# still resolve the bare commit/tree object by SHA (which is how the
# earlier grounding checks above "confirmed" it -- API object lookup does
# not require reachability, a plain `git clone` does).  The
# zephyrproject-rtos mirror, checked the same way, has NOT been rewound:
# `git ls-remote https://github.com/zephyrproject-rtos/zcbor.git` shows
# `9164bd18dcd88ff9d9ef98279501fc1093571017` as BOTH `HEAD` and
# `refs/heads/main` right now -- the exact commit west pins is that
# mirror's live tip. Point this recipe at the same remote west itself
# resolves to, not at the name on the tin.
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
# Canonical project homepage (NordicSemiconductor/zcbor); SRC_URI below
# fetches from a different remote -- see the UPSTREAM + PIN note above.
HOMEPAGE = "https://github.com/NordicSemiconductor/zcbor"
LICENSE = "Apache-2.0"
# md5 computed for real off the actual pinned tree's LICENSE file
# (`md5sum LICENSE` against the checkout at the SRCREV below), not
# guessed or carried over from another package -- same standard the
# onnxruntime recipe's compound LIC_FILES_CHKSUM holds itself to.
LIC_FILES_CHKSUM = "file://LICENSE;md5=3b83ef96387f14655fc854ddc3c6bd57"

# nobranch=1, NOT branch=main.  Belt-and-braces: the mirror's `main`
# happens to BE this exact commit right now (see the remote-choice note
# above), so `branch=main` would also fetch cleanly today -- but the
# whole reason a west-pinned SHA can go unreachable on a repo (as it did
# on the canonical upstream, above) is a subsequent rewrite of that same
# branch. nobranch=1 pins the fetch to the object, not to whatever `main`
# happens to point at on the day this recipe is built, so a later rewrite
# of the mirror's `main` can't silently break do_fetch here.
SRC_URI = "git://github.com/zephyrproject-rtos/zcbor.git;protocol=https;nobranch=1"
SRCREV = "9164bd18dcd88ff9d9ef98279501fc1093571017"
S = "${WORKDIR}/git"

# No `inherit cmake` (or any build-system inherit): there is nothing for
# one to drive, per the header comment above.  do_configure stays the
# empty default from base.bbclass.

do_compile() {
    for src in zcbor_common zcbor_decode zcbor_encode zcbor_print; do
        ${CC} ${CFLAGS} ${CPPFLAGS} -I${S}/include -fPIC \
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
        ${S}/include/zcbor_tags.h ${D}${includedir}/

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
