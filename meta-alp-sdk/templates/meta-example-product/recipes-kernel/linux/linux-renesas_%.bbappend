# SPDX-License-Identifier: Apache-2.0
# Carrier device tree on the STOCK MACHINE. e1m-v2n101-x-evk.dts /
# e1m-v2m101-x-evk.dts below shadow the SDK's files of the same name, so the
# dtb keeps the basename U-Boot's family table loads; KERNEL_DEVICETREE and
# the U-Boot fdtfile stay untouched. See docs/v2n-custom-carrier.md for a
# custom dtb name.
FILESEXTRAPATHS:prepend := "${THISDIR}/linux-renesas:"

SRC_URI:append = " file://example-carrier.dtsi"

# The SDK's do_configure:prepend copies the board dts and the stock dtsi into
# the kernel tree; add the carrier dtsi next to them.
do_configure:prepend() {
    install -m 0644 "${WORKDIR}/example-carrier.dtsi" \
        "${S}/arch/arm64/boot/dts/renesas/"
}
