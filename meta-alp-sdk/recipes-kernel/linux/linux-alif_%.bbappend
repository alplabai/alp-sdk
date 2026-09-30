# meta-alp-sdk: E1M-AEN EVK Linux console routing (#1979, Linux half).
#
# Companion to trusted-firmware-a_%.bbappend's :e1m-aen801/:e1m-aen701
# BL32 console knobs (#1979, shipped in PR #2485) -- that patch fixed
# only the TF-A half; this bbappend carries the separate Linux-side trap
# the same issue describes: Alif's devkit_ex_dct_defines.h hardcodes
# `UART2_STATUS "okay"`, so serial@4901a000 (UART2) enumerates as ttyS0
# and wins `console=ttyS0` no matter what `aliases.serial0` names -- the
# 8250 driver assigns ttySN by probe order, not by the DT alias. See
# recipes-kernel/linux/linux-alif/e1m-aen-evk-console.dtsi for the fix
# (disable &uart2, enable &uart5, alias serial0 -> &uart5, stdout-path
# serial0) and its TBD(alif-hw-config) label-confirmation note.
#
# INERT TODAY, same reason as the TF-A half (see that bbappend's own
# header): no Scarthgap meta-alif-ensemble exists publicly (#1968), and
# even a vendored devkit-ex-b0 branch is Scarthgap-incompatible (#1971),
# so neither MACHINE=e1m-aen801-a32 nor e1m-aen701-a32 parses -- there is
# no `linux-alif` recipe in any layer on bblayers.conf for this bbappend
# to attach to, and it never runs. Verified only statically: no vendor
# devkit-e8/-e7 dts is vendored in this tree to run dtc/cpp against, so
# the fragment was grep/reasoning-checked against the issue's own
# confirmed facts (serial@4901a000 = UART2, serial@4901d000 = UART5,
# both from #1979 and PR #2485) and against e1m-x-evk.dtsi's alias idiom
# (`serial0 = &scif;`). Once a Scarthgap linux-alif recipe is wired for
# e1m-aen801/e1m-aen701 (KERNEL_DEVICETREE in e1m-aen801-a32.conf /
# e1m-aen701-a32.conf is itself still `# TBD(alif-hw-config)`), #include
# this fragment from that board dts and confirm the &uart2/&uart5 labels
# against the real vendor dtsi first.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:e1m-aen801 = " file://e1m-aen-evk-console.dtsi"
SRC_URI:append:e1m-aen701 = " file://e1m-aen-evk-console.dtsi"
