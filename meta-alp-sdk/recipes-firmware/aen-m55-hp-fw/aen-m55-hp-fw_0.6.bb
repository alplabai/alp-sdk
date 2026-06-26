# SPDX-License-Identifier: Apache-2.0
#
# Install shell for the E1M-AEN801 M55-HP rpmsg producer firmware.
#
# The firmware itself is a PREBUILT Cortex-M55 ELF -- a binary blob, so
# it is NOT redistributed in the public meta-alp-sdk layer (the repo's
# top-level `*.elf` rule keeps it out of public git; see the
# classifying-public-vs-internal policy).  The ELF is built out-of-tree
# from examples/multicore/rpmsg-aen/m55_hp via:
#   west build -b alp_e1m_aen801_m55_hp_ae822fa0e5597ls0_rtss_hp \
#              examples/multicore/rpmsg-aen/m55_hp
# Build it with the Zephyr SDK so the `alp_backends_*` iterable sections
# land in their linker slots -- a gnuarmemb/system-script build emits
# orphan-section warnings and the alp_* backends may not register.
#
# To bake: place the built ELF at this recipe's files/m55_hp.elf (it is
# git-ignored by the top-level `*.elf` rule) -- or have alp-sdk-internal
# supply it -- then clear the skip below (e.g.
# `SKIP_RECIPE[aen-m55-hp-fw] = ""` in local/auto.conf).

SUMMARY = "E1M-AEN801 M55-HP rpmsg producer firmware (remoteproc)"
DESCRIPTION = "Cortex-M55-HP Zephyr firmware for the rpmsg-aen demo; \
               installed where alp-remoteproc scans so the A32 Linux \
               side can boot it over MHUv2.  Prebuilt artifact, built \
               out-of-tree and supplied by alp-sdk-internal / the integrator."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

# Default-skip: the public layer ships no ELF.  Clear this skip once the
# prebuilt firmware is placed at files/m55_hp.elf (scarthgap honors
# SKIP_RECIPE; PNBLACKLIST has been inert since honister).
SKIP_RECIPE[aen-m55-hp-fw] ?= "M55-HP firmware is a prebuilt binary not redistributed in the public layer; place the west-built ELF at files/m55_hp.elf (or have alp-sdk-internal supply it) and clear this skip to bake."
EXCLUDE_FROM_WORLD = "1"

SRC_URI = "file://m55_hp.elf"

do_install() {
    install -d ${D}/lib/firmware/alp/E1M-AEN801
    install -m 0644 ${WORKDIR}/m55_hp.elf \
        ${D}/lib/firmware/alp/E1M-AEN801/m55_hp.elf
}

FILES:${PN} = "/lib/firmware/alp/E1M-AEN801/m55_hp.elf"
COMPATIBLE_MACHINE = "e1m-aen801-a32"

# Prebuilt Cortex-M55 firmware: keep OE's host strip + sysroot strip off
# it (they would corrupt the cross-arch ELF -- including the
# alp_backends_* iterable sections), and tell the QA insane checker the
# arch mismatch (M55 ELF on an AArch64 sysroot) is expected.
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_SYSROOT_STRIP = "1"
INSANE_SKIP:${PN} = "arch"
