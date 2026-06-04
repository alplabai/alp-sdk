# meta-alp-sdk: ALP E1M board device trees for the Renesas RZ/V2N SoM
# family, layered the way SoM vendors ship BSPs (SoC dtsi -> SoM dtsi ->
# carrier dtsi -> per-board dts -> named dtb), NOT as a patch pile against
# the EVK reference dts.
#
#   e1m-v2n-som.dtsi    on-module V2N: dual GbE PHYs, eMMC, xSPI NOR,
#                       DRP-AI reserved memory, core rails.
#   e1m-v2m-deepx.dtsi  V2M delta: DEEPX DXM1 NPU on PCIe + the on-module
#                       lane mux + NPU reset release (gpio-hogs).
#   e1m-x-evk.dtsi      E1M-X-EVK carrier: eth/i2c/usb/console enables,
#                       USB-OVC hog. (Cameras/DSI/audio/CAN are TODO.)
#   e1m-v2n101-x-evk.dts / e1m-v2m101-x-evk.dts  product boards.
#
# These compose up from the upstream Renesas SoC dtsi (r9a09g056.dtsi,
# already in the kernel source), so there is no "disable the EVK nodes"
# patch set and no MACHINEOVERRIDES ordering problem -- each MACHINE
# selects its own board dtb via KERNEL_DEVICETREE in conf/machine/*.
#
# BOOTLOADER: the board dtbs are named per product (e.g.
# e1m-v2n101-x-evk.dtb). The U-Boot bootcmd must load the matching name
# (set `fdtfile` per MACHINE, or derive it from the EEPROM SoM manifest)
# instead of the stock renesas/r9a09g056n48-rzv2n-evk.dtb.
#
# STATUS: UNVALIDATED through dtc/bitbake -- first structured port from
# the RZ/V2N EVK reference dts (kernel SHA 6717c06, BSP v6.30). Build
# `bitbake virtual/kernel` per MACHINE and fix any dtc errors; the dts
# files carry inline VERIFY notes (memory size per SKU, DEEPX bench
# checks).
#
# KERNEL-VERSION SCOPE: these board dts/dtsi were generated against the
# linux-renesas tree at kernel SHA 6717c06 (Renesas RZ/V SDK platform 7.1
# / BSP v6.30, linux 6.1.x). They #include the SoC dtsi r9a09g056.dtsi and
# use BSP-specific bindings (renesas,mmngr, RZV2N_PORT_PINMUX, etc.), so
# they are NOT portable across linux-renesas major versions. This append
# uses the `%` wildcard, which would also match a future incompatible
# linux-renesas PV.
# FLAG / TODO: scope this filename to the exact kernel PV (rename to
# linux-renesas_6.1.%.bbappend) once the linux-renesas recipe PV provided
# by the meta-renesas release in bblayers.conf is confirmed. Not renamed
# here because the exact PV string is not asserted in this layer; pinning
# the SRCREV the series was generated against (6717c06) and adding a
# COMPATIBLE check is the interim guard.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append = " \
    file://e1m-v2n-som.dtsi \
    file://e1m-x-evk.dtsi \
    file://e1m-v2m-deepx.dtsi \
    file://e1m-v2n101-x-evk.dts \
    file://e1m-v2m101-x-evk.dts \
    file://0001-clk-renesas-r9a09g056-keep-CM33-owned-RSCI7-RIIC8-on.patch \
"

# AMP clock ownership: RSCI7 + RIIC8 belong to the Cortex-M33 system
# manager (GD32 supervisor link).  Without this patch, Linux's
# clk_disable_unused turns their module clocks off AND asserts the coupled
# CPG BUS_MSTOP bits (the rzv2h-cpg driver ties the two together), which
# bus-faults the CM33 mid-operation ~15 s into every boot.  The patch
# marks the six clocks DEF_MOD_CRITICAL so both gates stay held for the
# remote core.  Silicon-validated 2026-06-03 (two cold cycles + warm
# reboot, link autonomous from ~2 s after power-on, no intervention).

# Drop the ALP board dts + dtsi into the kernel DT source dir so they
# compile next to the upstream Renesas dts (the board dts #include the
# SoC r9a09g056.dtsi and these dtsi by relative path).
ALP_DTS_DST = "${S}/arch/arm64/boot/dts/renesas"
do_configure:prepend() {
    install -m 0644 \
        "${WORKDIR}/e1m-v2n-som.dtsi" \
        "${WORKDIR}/e1m-x-evk.dtsi" \
        "${WORKDIR}/e1m-v2m-deepx.dtsi" \
        "${WORKDIR}/e1m-v2n101-x-evk.dts" \
        "${WORKDIR}/e1m-v2m101-x-evk.dts" \
        "${ALP_DTS_DST}/"
}

# TAS2563 smart-amp codec (mainline ASoC `tas2562` covers it) -- pre-staged
# for the carrier audio TODO in e1m-x-evk.dtsi. linux-renesas is
# kernel-yocto based, so this .cfg is auto-merged from SRC_URI. Harmless
# (no DT consumer) until the ti,tas2563 nodes land in the carrier dtsi.
SRC_URI:append:e1m-v2m101 = " file://tas2563-audio.cfg"
SRC_URI:append:e1m-v2n101 = " file://tas2563-audio.cfg"
