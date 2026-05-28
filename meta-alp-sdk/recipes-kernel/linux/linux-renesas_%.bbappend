# meta-alp-sdk: E1M-X carrier device-tree customizations for the
# Renesas RZ/V2N SoM family (E1M-V2N101 / E1M-V2N102).
#
# These re-home the patch set prototyped + HW-validated during V2N
# bring-up. With this bbappend you build against STOCK meta-renesas --
# do NOT also keep the same patches in a meta-renesas fork or they will
# double-apply.
#
# They patch the in-tree r9a09g056n48-rzv2n-evk.dts so the dtb the
# shipped bootloader already loads (renesas/r9a09g056n48-rzv2n-evk.dtb)
# becomes the e1m-x carrier dtb. Validated end-to-end on hardware
# (RZ/V2N r9a09g056n48, linux-renesas 6.1.141-cip43 @ AI SDK platform 7.1 / BSP v6.30, kernel
# SHA 6717c06c): model/compatible, EVK-only peripheral disables,
# RTL8211F-VD PHYs at MDIO addr 2, RIIC3/6/7 disabled, EVK audio off,
# USB OVC pins disabled (USB2.0 host kept working).
#
# Covers e1m-v2n101-a55 + e1m-v2n102-a55 (shared `e1m-v2n101` override).
# V2M (DEEPX) SKUs reuse the same deltas against a separate
# e1m-x-evk-v2m.dts target -- TODO once those boards are exercised.
#
# STATUS: patch CONTENT is HW-validated; this bbappend WIRING has not
# been run through bitbake yet -- confirm the patches apply against the
# linux-renesas SRCREV this layer pins (they were generated at
# 6717c06c) and adjust the override key if your MACHINEOVERRIDES differ.
#
# NOTE on the standalone carrier dts: meta-alp-sdk's machine confs
# originally had a placeholder `KERNEL_DEVICETREE:append =
# " renesas/e1m-x-evk.dts"`. We instead patch the rzv2n-evk dtb because
# the production bootloader's bootcmd loads that filename. Moving to a
# distinct renesas/e1m-x-evk.dtb is a clean follow-up that ALSO requires
# the bootloader bootcmd to load the new name (see the bootloader
# landing / docs/bring-up-v2n.md).

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:e1m-v2n101 = " \
    file://0006-arm64-dts-renesas-rzv2n-evk-customize-for-e1m-x_v2n-.patch \
    file://0007-arm64-dts-renesas-rzv2n-evk-disable-absent-peripheral.patch \
    file://0008-arm64-dts-renesas-rzv2n-evk-swap-eth-phy-to-rtl8211fdi.patch \
    file://0009-arm64-dts-renesas-rzv2n-evk-correct-eth-phy-id-rtl-fvd.patch \
    file://0010-arm64-dts-renesas-rzv2n-evk-disable-absent-audio-on-e1m-x.patch \
    file://0011-arm64-dts-renesas-rzv2n-evk-disable-unrouted-RIIC3-6-7-on-e1m-x.patch \
    file://0012-arm64-dts-renesas-rzv2n-evk-disable-USB-OVC-pins-on-e1m-x.patch \
    file://0013-arm64-dts-renesas-rzv2n-evk-set-eth-phy-mdio-addr-to-2-on-e1m-x.patch \
    file://tas2563-audio.cfg \
"

# Kernel config fragment for the carrier's TAS2563 smart-amp codec.  The
# mainline ASoC `tas2562` driver covers the TAS2563 and is NOT in the
# linux-renesas 6.1-cip43 defconfig, so merge CONFIG_SND_SOC_TAS2562=y in.
# Pre-stages the driver: DT patch 0010 only DISABLES the EVK DA7212 audio
# graph; the ti,tas2563 codec nodes + audio-graph-card land in a follow-up
# patch (0014, not yet merged), at which point this driver gets a consumer.
#
# STATUS: UNVALIDATED through bitbake -- confirm the linux-renesas config
# flow during the build pass.  merge_config.sh is the canonical kernel
# fragment merger; if linux-renesas is kernel-yocto based, the .cfg in
# SRC_URI is already merged and this append is a harmless no-op.
do_configure:append:e1m-v2n101() {
    if [ -f "${WORKDIR}/tas2563-audio.cfg" ]; then
        "${S}/scripts/kconfig/merge_config.sh" -m -O "${B}" \
            "${B}/.config" "${WORKDIR}/tas2563-audio.cfg"
        oe_runmake -C "${S}" O="${B}" olddefconfig
    fi
}
