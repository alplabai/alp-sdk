# SPDX-License-Identifier: Apache-2.0
#
# Dev-only: installs the Cortex-M33 system-manager ELF as /lib/firmware/m33_sm.elf
# so Linux remoteproc can reload it (echo m33_sm.elf > .../firmware; echo start
# > .../state).  Built only with ALP_V2N_CM33_SRAM_NS = "1" (decision Q53), the
# flag that also opens CM33 SRAM to Linux in TF-A and drops alp,rz-attach-only
# from the cm33_rproc node; with the default "0" the recipe is skipped.
#
# ALP_CM33_ELF points at the zephyr.elf built out of tree, e.g.
#   west build -b alp_e1m_v2m101_m33_sm/r9a09g056n48gbg/cm33 examples/multicore/rpmsg-v2n/m33_sm
#   ALP_CM33_ELF = "/path/to/build/zephyr/zephyr.elf"
# It must be the SAME build as the xSPI image TF-A boots.

SUMMARY = "CM33 system-manager ELF for Linux remoteproc reload (dev only)"
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

ALP_V2N_CM33_SRAM_NS ??= "0"
ALP_CM33_ELF ??= ""

python () {
    if d.getVar('ALP_V2N_CM33_SRAM_NS') != '1':
        raise bb.parse.SkipRecipe('ALP_V2N_CM33_SRAM_NS is not "1" (dev-only recipe)')
}

inherit allarch
S = "${WORKDIR}"

do_install() {
    [ -f "${ALP_CM33_ELF}" ] || bbfatal "set ALP_CM33_ELF to the CM33 zephyr.elf"
    install -Dm 0644 "${ALP_CM33_ELF}" ${D}${nonarch_base_libdir}/firmware/m33_sm.elf
}

FILES:${PN} = "${nonarch_base_libdir}/firmware/m33_sm.elf"
COMPATIBLE_MACHINE = "(e1m-v2n.*|e1m-v2m.*)"
