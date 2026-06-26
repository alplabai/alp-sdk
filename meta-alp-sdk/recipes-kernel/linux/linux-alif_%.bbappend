# meta-alp-sdk: ALP E1M-AEN801 carrier device tree for the Alif Ensemble E8
# A32 cluster, layered OVER the Alif `linux-alif` recipe (meta-alif-ensemble)
# per ADR 0017 -- we do NOT fork the kernel; we add only the carrier board dts.
#
# Composition mirrors the Alif devkit model (arch/arm, aarch32):
#   e1m-aen801-evk.dts  #includes ../common/ensemble-ex.dtsi (E8 SoC, consumed)
#                       + e1m_dct_defines.h (our *_STATUS macro header).
#
# STATUS: BASELINE. e1m-aen801-evk.dts + e1m_dct_defines.h are derived verbatim
# from the Alif devkit-e8 peripheral selection so the build chain
# (env -> kernel -> our dtb) is proven end-to-end on real sources. The E1M-EVK
# peripheral selection (console UART instance, carrier I2C buses + devices, OSPI
# NOR/HyperRAM, MHU to the M55s for SP3) is refined against the E1M-EVK -> Alif
# HW mapping (metadata/boards/e1m-evk.yaml) once provided -- see
# docs/superpowers/specs/2026-06-25-aen-a32-yocto-bringup-design.md (§12b).

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://e1m-aen801-evk.dts \
    file://e1m_dct_defines.h \
    file://aen801-dts-reservations.dtsi \
"

# linux-alif pins COMPATIBLE_MACHINE to "(devkit-e).*|(appkit-e).*"; extend it to
# our E1M carrier machines so the recipe builds for e1m-aen801-a32.  (:append
# concatenates with no separator, so the regex stays well-formed.)
COMPATIBLE_MACHINE:append = "|(e1m-).*"

# Drop the ALP carrier dts + its defines header into a dedicated subdir next to
# the Alif devkit/appkit boards (the dts #includes ../common/ensemble-ex.dtsi by
# relative path).  KERNEL_DEVICETREE in conf/machine/e1m-aen801-a32.conf points
# at alif/ensemble/e1m/e1m-aen801-evk.dtb.
ALP_DTS_DST = "${S}/arch/arm/boot/dts/alif/ensemble/e1m"
ALP_ENSEMBLE_MK = "${S}/arch/arm/boot/dts/alif/ensemble/Makefile"
do_configure:prepend() {
    install -d "${ALP_DTS_DST}"
    install -m 0644 "${WORKDIR}/e1m-aen801-evk.dts" "${WORKDIR}/e1m_dct_defines.h" \
        "${WORKDIR}/aen801-dts-reservations.dtsi" "${ALP_DTS_DST}/"
    # Register the carrier dtb: the Alif kernel uses subdir Makefiles
    # (ensemble/Makefile descends into devkit/ + appkit/ via subdir-y, each with
    # its own dtb-y list).  Drop an e1m/Makefile and make ensemble descend into
    # it -- without this `make .../e1m/e1m-aen801-evk.dtb` has "no rule".
    printf '%s\n%s\n' '# SPDX-License-Identifier: GPL-2.0' \
        'dtb-y += e1m-aen801-evk.dtb' > "${ALP_DTS_DST}/Makefile"
    if ! grep -q 'subdir-y += e1m' "${ALP_ENSEMBLE_MK}" ; then
        echo 'subdir-y += e1m' >> "${ALP_ENSEMBLE_MK}"
    fi
}
