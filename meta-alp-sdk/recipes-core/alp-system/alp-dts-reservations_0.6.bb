# SPDX-License-Identifier: Apache-2.0
#
# Yocto recipe that ships the orchestrator-emitted
# `dts-reservations.dtsi` into the kernel build's DTS include path
# so the Linux device-tree picks up the same alp_default_rpmsg /
# customer-app carve-outs the Zephyr side sees.
#
# Inputs:
#   - ${ALP_SYSTEM_MANIFEST_PATH}: path to the orchestrator's
#     system-manifest.yaml (typically build/system-manifest.yaml in
#     the alp-sdk workspace).  When unset the recipe looks at
#     ${TOPDIR}/../alp-sdk/build/system-manifest.yaml.
#   - The .dtsi the manifest points at (typically build/generated/
#     dts-reservations.dtsi alongside the manifest).
#
# Output:
#   - Installs the .dtsi at
#     ${STAGING_DIR_HOST}${datadir}/alp-sdk/dts/alp-dts-reservations.dtsi
#     so a kernel .bbappend can pull it into KERNEL_DEVICETREE via
#     `SRC_URI += "file://alp-dts-reservations.dtsi"` and #include it
#     from the per-MACHINE DT.
#
# Phase 3 carve-out: this recipe makes the .dtsi available to the
# kernel-build's DT pre-processor.  Wiring it into the actual per-
# MACHINE DT files (V2N's renesas DT, AEN's alif DT, etc.) is a
# per-BSP layer concern handled by Phase 4 (Wave 4 owns the per-
# example board.yaml + BSP integration).

SUMMARY = "ALP SDK heterogeneous-IPC carve-out DTS reservations"
DESCRIPTION = "Bridges the orchestrator-emitted dts-reservations.dtsi \
               from build/generated into the Yocto kernel DT pipeline \
               so the Linux + Zephyr sides see identical alp_default_rpmsg \
               (and customer-defined) memory carve-outs.  Required when \
               <alp/rpc.h> is used from the A-class side."
HOMEPAGE = "https://github.com/alplabai/alp-sdk"
LICENSE = "Apache-2.0"
LIC_FILES_CHKSUM = "file://${COMMON_LICENSE_DIR}/Apache-2.0;md5=89aea4e17d99a7cacdbeed46a0096b10"

# Customers override on the bitbake command line:
#   bitbake -e ALP_SYSTEM_MANIFEST_PATH=/work/alp-sdk/build/system-manifest.yaml ...
# A sane default points one workspace level above (the typical
# layout: alp-sdk/ + meta-alp-sdk/ siblings).
ALP_SYSTEM_MANIFEST_PATH ??= "${TOPDIR}/../alp-sdk/build/system-manifest.yaml"

inherit allarch

S = "${WORKDIR}"

python do_alp_resolve_manifest() {
    import os
    manifest = d.getVar('ALP_SYSTEM_MANIFEST_PATH', True)
    if not manifest or not os.path.isfile(manifest):
        bb.fatal(
            "alp-dts-reservations: expected the orchestrator's "
            "system-manifest.yaml at '%s'.  Build with `west alp-build` "
            "first or override ALP_SYSTEM_MANIFEST_PATH on the bitbake "
            "command line." % manifest)

    # The .dtsi sits alongside the manifest under build/generated/.
    dtsi = os.path.join(os.path.dirname(manifest),
                        'generated', 'dts-reservations.dtsi')
    if not os.path.isfile(dtsi):
        bb.fatal(
            "alp-dts-reservations: expected dts-reservations.dtsi at "
            "'%s' (computed from ALP_SYSTEM_MANIFEST_PATH).  Re-run the "
            "orchestrator to regenerate it." % dtsi)
    d.setVar('ALP_DTS_RESERVATIONS_PATH', dtsi)
}
addtask alp_resolve_manifest before do_install after do_unpack

do_install() {
    install -d ${D}${datadir}/alp-sdk/dts
    install -m 0644 ${ALP_DTS_RESERVATIONS_PATH} \
        ${D}${datadir}/alp-sdk/dts/alp-dts-reservations.dtsi
}

FILES:${PN} = "${datadir}/alp-sdk/dts/alp-dts-reservations.dtsi"

COMPATIBLE_MACHINE = "(e1m-v2n.*|e1m-v2m.*|e1m-aen.*|e1m-nx9101.*)"
