# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe for the Murata LBEE5HY2FY (Type 2GY) module-specific NVRAM
# + CLM calibration blob, installed under brcmfmac's BOARD-SPECIFIC names
# so they are picked ahead of cyw-fmac-firmware's generic
# cypress/cyfmac55500-sdio.{txt,clm_blob} (recipes-kernel/cyw-fmac-firmware/)
# without overwriting them.
#
# brcmfmac's SDIO firmware-name lookup tries, in order:
#   cypress/cyfmac55500-sdio.<first root DT "compatible">.txt / .clm_blob
#   cypress/cyfmac55500-sdio.txt / .clm_blob                    (generic)
# The generic pair cyw-fmac-firmware installs today is the Infineon
# eval-board NVRAM (boardtype 0x0899, placeholder MAC 00:90:4c:xx:xx:xx) --
# correct for nothing this SDK ships. This recipe supplies the real
# per-module blobs for the on-module Murata LBEE5HY2FY (Type 2GY), keyed
# to the board compatible strings so every EVK variant this layer ships
# a board dts for picks them up automatically, with the generic files
# left alone as a fallback for any board this recipe doesn't yet cover.
#
# Bench-verified 2026-09-26 (E1M-V2M103 unit 2026W38-0001, root compatible
# "alp,e1m-v2m101-x-evk" -- see e1m-v2m101-x-evk.dts; only the *101 board
# dts exist in this layer today, so both V2N and V2M SKUs currently
# enumerate under one of those two compatibles): scan, WPA2/SAE
# association on 5 GHz channel 60, DHCP.

SUMMARY     = "Murata LBEE5HY2FY (Type 2GY) module NVRAM + CLM for cyw-fmac"
DESCRIPTION = "Module-specific NVRAM config and STAIndoor CLM calibration \
blob for the on-module Murata LBEE5HY2FY-922 (Infineon CYW55513), Type 2GY, \
installed under brcmfmac's per-board-compatible firmware names so they are \
selected ahead of cyw-fmac-firmware's generic Infineon eval-board blobs."
HOMEPAGE    = "https://github.com/murata-wireless"
SECTION     = "kernel"

# Same upstream licence as recipes-kernel/cyw-fmac-firmware/ (Cypress
# firmware EULA: reproduce + distribute in object-code form, solely for
# use with Cypress/Infineon ICs -- exactly this use). Byte-identical
# LICENCE.cypress/LICENCE text across both upstream repos, same md5 this
# layer already accepts for the sibling recipe.
LICENSE                              = "Firmware-cypress"
NO_GENERIC_LICENSE[Firmware-cypress] = "LICENCE.cypress"
LIC_FILES_CHKSUM = " \
    file://${MURATA_NVRAM_S}/LICENCE.cypress;md5=cbc5f665d04f741f1e006d2096236ba7 \
    file://${MURATA_FW_S}/LICENCE;md5=cbc5f665d04f741f1e006d2096236ba7 \
"

# Two upstream repos, each unpacked into its own destsuffix.
SRC_URI = " \
    git://github.com/murata-wireless/cyw-fmac-nvram;protocol=https;branch=master;name=nvram;destsuffix=murata-2gy-nvram \
    git://github.com/murata-wireless/cyw-fmac-fw;protocol=https;branch=master;name=fw;destsuffix=murata-2gy-fw \
"
SRCREV_FORMAT = "nvram_fw"
# Current master tip at authoring time (2026-09-26); re-verify with
# `gh api repos/murata-wireless/<repo>/commits/master` before re-pinning.
SRCREV_nvram = "40a917f1a50f9e4df44a7ec63901771791d53f09"
# cyw-fmac-fw is shared with recipes-kernel/cyw-fmac-firmware/, which
# already pins this exact commit for its own (2FY) CLM blob -- reuse the
# same SRCREV rather than drifting a second pin for the same repo.
SRCREV_fw    = "ff2dcdb5137de70db9a741ef78346230660459c2"

PV = "2gy+git${SRCPV}"

MURATA_NVRAM_S = "${WORKDIR}/murata-2gy-nvram"
MURATA_FW_S    = "${WORKDIR}/murata-2gy-fw"
S = "${MURATA_NVRAM_S}"

# Source blob names (module-suffixed) as published upstream.
NVRAM_SRC = "cyfmac55500-sdio.2GY.txt"
CLM_SRC   = "cyfmac55500-sdio.2GY.STAIndoor.clm_blob"

# Board root-compatible strings this layer ships a board dts for (see
# meta-alp-sdk/recipes-kernel/linux/linux-renesas/e1m-v2n101-x-evk.dts and
# e1m-v2m101-x-evk.dts). Extend this list the day a *102/*103 board dts is
# added -- COMPATIBLE_MACHINE on cyw-fmac_git.bb already covers those
# MACHINEs, but no board dts exists for them yet, so there is no
# compatible string to key a board-specific NVRAM name on until one lands.
MURATA_2GY_COMPATIBLES = "alp,e1m-v2n101-x-evk alp,e1m-v2m101-x-evk"

do_install() {
    install -d ${D}${nonarch_base_libdir}/firmware/cypress
    for compat in ${MURATA_2GY_COMPATIBLES}; do
        install -m 0644 ${MURATA_NVRAM_S}/${NVRAM_SRC} \
            ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.${compat}.txt
        install -m 0644 ${MURATA_FW_S}/${CLM_SRC} \
            ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.${compat}.clm_blob
    done
}

FILES:${PN} = "${nonarch_base_libdir}/firmware"

# Firmware blobs are architecture-independent data.
inherit allarch

INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_SYSROOT_STRIP = "1"
INSANE_SKIP:${PN} += "arch"

# Same gate as the driver: only the SKUs that actually carry the Murata
# module.
COMPATIBLE_MACHINE = "e1m-v2n101-a55|e1m-v2n102-a55|e1m-v2n103-a55|e1m-v2m101-a55|e1m-v2m102-a55|e1m-v2m103-a55"
