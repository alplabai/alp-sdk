# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe for the Infineon/Cypress CYW55513 (Murata Type 2FY)
# Wi-Fi + Bluetooth firmware blobs used by the cyw-fmac driver on the
# Alp Lab E1M V2N / V2M SoM family.
#
# FIVE blobs ship across FOUR upstream repos. The brcmfmac driver loads
# the GENERIC chip+bus names (cyfmac55500-sdio.{trxse,clm_blob,txt})
# from /lib/firmware/cypress/ at probe -- NOT the module-suffixed source
# names -- so the CLM blob and NVRAM are renamed on install. btbcm /
# the userspace BT load path consumes the .hcd patch from
# /lib/firmware/brcm/; both regulatory variants (FCC and CE/JP) are
# staged so the DT firmware-name (or userspace tool) selects per region.
#
#   1) cyfmac55500-sdio.trxse        WLAN radio firmware (Infineon ifx repo)
#   2) cyfmac55500-sdio.clm_blob     regulatory CLM, STAIndoor default (cyw-fmac-fw)
#   3) cyfmac55500-sdio.txt          NVRAM (cyw-fmac-nvram)
#   4) CYW55500A1_...FCC...2FY.2GY.hcd   BT patch, FCC region (cyw-bt-patch)
#   5) CYW55500A1_...CE.JP...2FY.2GY.hcd BT patch, CE/JP region (cyw-bt-patch)
#
# The WLAN radio firmware (.trxse) is NOT bundled in any murata-wireless
# repo; it comes from Infineon ifx-linux-firmware, pinned in LOCKSTEP
# with the cyw-fmac backports base (release-v6.1.97-2024_1115, the
# "Jaculus" release == the imx-mickledore-jaculus backports checkout).
#
# Co-located under recipes-kernel/ (not recipes-bsp/) alongside the
# cyw-fmac driver recipe this pairs with -- both are Wi-Fi/BT-kernel
# concerns for the same on-module part.

SUMMARY     = "Infineon CYW55513 (Murata 2FY) Wi-Fi/BT firmware blobs"
DESCRIPTION = "WLAN radio firmware (.trxse), regulatory CLM blob, NVRAM, and \
Bluetooth .hcd patch (FCC + CE/JP) for the Infineon CYW55513 / Murata Type 2FY \
module on the Alp Lab E1M V2N / V2M SoM. Paired with the cyw-fmac driver."
HOMEPAGE    = "https://github.com/murata-wireless"
SECTION     = "kernel"

# Cypress firmware licence (identical across all four upstream repos).
# The on-disk filename differs per repo (LICENCE vs LICENCE.cypress);
# the checksum is the same. No generic mapping exists for it.
LICENSE                          = "Firmware-cypress"
NO_GENERIC_LICENSE[Firmware-cypress] = "LICENCE"
LIC_FILES_CHKSUM = "file://${CYW_FW_S}/LICENCE;md5=cbc5f665d04f741f1e006d2096236ba7"

# Four upstream repos, each unpacked into its own destsuffix so the
# do_install picks the right blob from the right tree.
SRC_URI = " \
    git://github.com/murata-wireless/cyw-fmac-fw;protocol=https;branch=master;name=fw;destsuffix=cyw-fmac-fw \
    git://github.com/murata-wireless/cyw-fmac-nvram;protocol=https;branch=master;name=nvram;destsuffix=cyw-fmac-nvram \
    git://github.com/murata-wireless/cyw-bt-patch;protocol=https;branch=master;name=bt;destsuffix=cyw-bt-patch \
    git://github.com/Infineon/ifx-linux-firmware;protocol=https;nobranch=1;name=ifx;destsuffix=ifx-linux-firmware \
"
# ifx-linux-firmware: SRCREV_ifx below is the commit the release-v6.1.97-2024_1115
# tag dereferences to. Deliberately NOT also passing `;tag=...` on the URL
# above -- bitbake's git fetcher rejects a URL `tag=`/`rev=` alongside an
# explicit SRCREV for the same name as a "conflicting revisions" parse error
# (caught during a `bitbake -p` dry-run 2026-09-25). SRCREV alone pins it;
# the tag name is recorded here purely for humans re-deriving the SRCREV.

SRCREV_FORMAT = "fw_nvram_bt_ifx"
SRCREV_fw     = "ff2dcdb5137de70db9a741ef78346230660459c2"
SRCREV_nvram  = "411c87d4cf924a1a5415273265fd54d7d7d4044f"
SRCREV_bt     = "64ac86708253e12d7089cf75ef8dcc9b30594958"
# ifx-linux-firmware release-v6.1.97-2024_1115 is a TAG (no same-named
# branch); SRCREV is the commit the tag dereferences to. trxse at this
# tag: md5 d48a2e56ee4423fc23f282a257614b22, 511132 bytes.
SRCREV_ifx    = "fde0d5a819bf37aeee6c911099ec85bdbf2bb28d"

PV = "6.1.97+git${SRCPV}"

# Convenience handles to each unpacked tree.
CYW_FW_S    = "${WORKDIR}/cyw-fmac-fw"
CYW_NVRAM_S = "${WORKDIR}/cyw-fmac-nvram"
CYW_BT_S    = "${WORKDIR}/cyw-bt-patch"
IFX_FW_S    = "${WORKDIR}/ifx-linux-firmware"

S = "${WORKDIR}/cyw-fmac-fw"

# Source blob names (module-suffixed) vs. driver-expected install names.
CLM_SRC   = "cyfmac55500-sdio.2FY.STAIndoor.clm_blob"
NVRAM_SRC = "cyfmac55500-sdio.2FY.txt"
HCD_FCC   = "CYW55500A1_001.002.032.0040.0033.FCC.2FY.2GY.hcd"
HCD_CEJP  = "CYW55500A1_001.002.032.0040.0032.CE.JP.2FY.2GY.hcd"

do_install() {
    # WLAN: radio firmware + regulatory CLM + NVRAM -> /lib/firmware/cypress/
    # (brcmfmac loads the generic chip+bus names; rename CLM/NVRAM on install.)
    install -d ${D}${nonarch_base_libdir}/firmware/cypress
    install -m 0644 ${IFX_FW_S}/firmware/cyfmac55500-sdio.trxse \
        ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.trxse
    install -m 0644 ${CYW_FW_S}/${CLM_SRC} \
        ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.clm_blob
    install -m 0644 ${CYW_NVRAM_S}/${NVRAM_SRC} \
        ${D}${nonarch_base_libdir}/firmware/cypress/cyfmac55500-sdio.txt

    # Bluetooth: both regulatory .hcd variants -> /lib/firmware/brcm/
    # (DT firmware-name or the userspace BT load flow picks the region).
    install -d ${D}${nonarch_base_libdir}/firmware/brcm
    install -m 0644 ${CYW_BT_S}/${HCD_FCC} \
        ${D}${nonarch_base_libdir}/firmware/brcm/${HCD_FCC}
    install -m 0644 ${CYW_BT_S}/${HCD_CEJP} \
        ${D}${nonarch_base_libdir}/firmware/brcm/${HCD_CEJP}
}

FILES:${PN} = "${nonarch_base_libdir}/firmware"

# Firmware blobs are architecture-independent data.
inherit allarch

# Defensive; non-ELF payloads never trip arch/strip QA, but these are
# explicit for clarity and future-proofing if a build host's QA config changes.
INHIBIT_PACKAGE_STRIP = "1"
INHIBIT_SYSROOT_STRIP = "1"
INSANE_SKIP:${PN} += "arch"
