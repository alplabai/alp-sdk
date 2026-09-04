# meta-alp-sdk: ALP E1M board device trees for the Renesas RZ/V2N SoM
# family, layered the way SoM vendors ship BSPs (SoC dtsi -> SoM dtsi ->
# carrier dtsi -> per-board dts -> named dtb), NOT as a patch pile against
# the EVK reference dts.
#
#   e1m-v2n-som.dtsi    on-module V2N: dual GbE PHYs, eMMC, xSPI NOR,
#                       DRP-AI reserved memory, core rails.
#   e1m-v2n-drpai.dtsi  the &drpai0 enable that claims that reserved memory.
#                       Installed ONLY when meta-rz-drpai is in bblayers
#                       (it creates the label); stubbed out otherwise --
#                       see the ALP_DRPAI_DT_ENABLE block below.
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

# Deterministic, branded kernel banner.  Poky's linux-kernel-base sets
# KBUILD_BUILD_USER/HOST to "oe-user"/"oe-host" by default, which is
# already reproducible -- but a manually-built bench kernel (built
# outside bitbake) leaks the developer's real user@host into the boot
# banner ("Linux version ... (caner@DESKTOP-...)").  Pin both to an ALP
# id so the shipped banner is branded and a manual build that inherits
# the recipe environment can never leak.  (A manual kernel build OUTSIDE
# bitbake does not source this recipe -- it must export the same two
# vars itself: see docs/build-yocto-v2n.md "Hand-building the kernel".)
export KBUILD_BUILD_USER = "alp"
export KBUILD_BUILD_HOST = "alp-sdk"

SRC_URI:append = " \
    file://e1m-v2n-som.dtsi \
    file://e1m-x-evk.dtsi \
    file://e1m-v2m-deepx.dtsi \
    file://e1m-v2n101-x-evk.dts \
    file://e1m-v2m101-x-evk.dts \
    file://0001-clk-renesas-r9a09g056-keep-CM33-owned-RSCI7-RIIC8-on.patch \
    file://0002-drm-renesas-rzg2l-mipi-dsi-pm_runtime-guard-host-tra.patch \
    file://0003-usb-ohci-platform-add-spurious-oc-DT-property.patch \
    file://0004-drm-panel-add-himax-hx8394-with-rocktech-rk055hdmipi.patch \
    file://0005-gpio-add-gd32-bridge-expander-driver.patch \
    file://0006-input-goodix-fall-back-to-polling-without-an-irq.patch \
"

# AMP clock ownership: RSCI7 + RIIC8 belong to the Cortex-M33 system
# manager (GD32 supervisor link).  Without this patch, Linux's
# clk_disable_unused turns their module clocks off AND asserts the coupled
# CPG BUS_MSTOP bits (the rzv2h-cpg driver ties the two together), which
# bus-faults the CM33 mid-operation ~15 s into every boot.  The patch
# marks the six clocks DEF_MOD_CRITICAL so both gates stay held for the
# remote core.  Silicon-validated 2026-06-03 (two cold cycles + warm
# reboot, link autonomous from ~2 s after power-on, no intervention).

# 0002 (DSI shutdown SError): rzg2l_mipi_dsi's host transfer touched DSI
# registers while the host was runtime-suspended (held in reset).  A panel
# .shutdown() that disables the panel during device_shutdown() -- the
# E1M-X LCD's hx8394 -- then took an asynchronous SError -> kernel panic ->
# the reboot never completed and the board hung.  (The SoC reset path
# itself is fine: sysrq-b, which skips device_shutdown, resets cleanly via
# PSCI/WDT.)  The patch pm_runtime-resumes the host around the register
# accesses so a DCS transfer is safe in any PM state; the bounded
# completion poll just times out harmlessly when the link is down.
# Silicon-validated 2026-06-11 on E1M-V2M101: `reboot` now reaches the
# reset and boots.

# 0003 (usb20 OVC silencing): the carrier's OVC sense is unusable
# (errata E3) and P9.6 can no longer be parked as GPIO (CM33-owned
# SCK7), so OC is suppressed at the controllers instead.  EHCI already
# has the generic spurious-oc DT property in-tree; 0003 adds the same
# property to ohci-platform (sets NOCP / clears OCPM in roothub A) and
# documents it in generic-ohci.yaml.  Both &ehci0 and &ohci0 carry
# spurious-oc in e1m-x-evk.dtsi.  Cold-boot-verified 2026-06-12 on
# E1M-V2M101: zero over-current lines.

# DRP-AI3 NPU overlay -- CONDITIONAL on the OPTIONAL meta-rz-drpai layer.
#
# e1m-v2n-som.dtsi #includes e1m-v2n-drpai.dtsi unconditionally; this block
# decides which body that filename gets:
#
#   layer in bblayers.conf AND ALP_ENABLE_DRPAI = "1"
#                                  -> the real `&drpai0` override
#   either condition unmet         -> a comment-only stub (no node touched)
#
# Both are required.  meta-rz-drpai ships bundled in the RZ/V2N AI SDK BSP,
# so keying off its presence alone would flip the NPU on for every V2N/V2M
# image whether or not the owner asked for one.
#
# The `drpai0` LABEL does not exist in the pristine linux-renesas tree.  It
# is CREATED by meta-rz-drpai's
# recipes-kernel/linux/linux-renesas/0001-add-drpai-property-to-devicetree.patch,
# which adds `drpai0: drpai@16800000 { ... status = "disabled"; }` to
# r9a09g056.dtsi; that layer's 0002 patch adds the driver behind it.
# meta-rz-drpai is only LAYERRECOMMENDS_alp-sdk -- a SOFT dep -- so without
# this guard a bake that drops it dies in dtc on an unresolved reference,
# and it takes the V2M dtb down with the V2N one (both board dts include
# e1m-v2n-som.dtsi).
#
# Chosen over promoting meta-rz-drpai to LAYERDEPENDS_alp-sdk: a hard dep
# would make an RZ/V-only vendor layer mandatory for EVERY meta-alp-sdk
# consumer, including the e1m-aen801-a32 / e1m-nx9101-a55 machines that have
# no DRP-AI silicon at all and never build linux-renesas.  This keeps the
# blast radius inside the one recipe that actually compiles the node.
#
# Guarded on the LAYER because the layer is what supplies both the label and
# the driver -- every V2N/V2M SKU carries the same DRP-AI3, so there is no
# per-SKU axis here.
ALP_DRPAI_LAYER = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', '1', '0', d)}"
# Hash on the resolved 0/1, not on BBFILE_COLLECTIONS: bb.utils.contains makes
# bitbake add the whole collection list to do_configure's signature otherwise,
# so adding ANY unrelated layer would re-run the kernel configure.
ALP_DRPAI_LAYER[vardepvalue] = "${ALP_DRPAI_LAYER}"

# ...but the layer alone is NOT enough to justify flipping the node on.
#
# meta-rz-drpai ships bundled in the RZ/V2N AI SDK BSP v6.30 package (see
# conf/layer.conf), so it is in the normal bblayers set for anyone building
# V2N at all.  Gating only on its presence would install this override --
# and take &drpai0 from "disabled" to "okay" -- on EVERY existing V2N/V2M
# image, so the driver would probe and /dev/drpai0 would appear on boards
# whose owners never asked for it.  That is a behaviour change disguised as
# an opt-in feature.
#
# So require an explicit ALP_ENABLE_DRPAI too, defaulting to 0.  It is
# DECLARED in all four V2N/V2M machine confs (`ALP_ENABLE_DRPAI ?= "0"`)
# next to ALP_ENABLE_DEEPX_DXM1, so a builder reading the conf for their
# MACHINE finds it -- the `??=` here is only the fallback for a consumer
# that uses this bbappend without one of those confs.  Turning the SDK backend on
# (PACKAGECONFIG "drpai") and turning the kernel node on are deliberately
# separate switches: the backend without the node fails at open() with a
# clear error, whereas the node without the backend is simply an idle
# device -- neither silently half-works.
ALP_ENABLE_DRPAI ??= "0"
ALP_DRPAI_DT_ENABLE = "${@'1' if (d.getVar('ALP_DRPAI_LAYER') == '1' and d.getVar('ALP_ENABLE_DRPAI') == '1') else '0'}"
ALP_DRPAI_DT_ENABLE[vardepvalue] = "${ALP_DRPAI_DT_ENABLE}"
SRC_URI += "${@' file://e1m-v2n-drpai.dtsi' if d.getVar('ALP_DRPAI_DT_ENABLE') == '1' else ''}"

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

    # Branch on the bitbake variable, not on the presence of the unpacked
    # file: dropping meta-rz-drpai from bblayers.conf does not scrub a
    # previously-unpacked ${WORKDIR}, so a file test would keep emitting the
    # real override into a tree that no longer has the label.
    if [ "${ALP_DRPAI_DT_ENABLE}" = "1" ]; then
        install -m 0644 "${WORKDIR}/e1m-v2n-drpai.dtsi" "${ALP_DTS_DST}/"
    else
        printf '%s\n' \
            '/* DRP-AI3 NPU node not claimed in this build.' \
            ' *' \
            ' * Needs BOTH meta-rz-drpai in bblayers.conf (it supplies the' \
            ' * &drpai0 label and the driver) AND ALP_ENABLE_DRPAI = "1".' \
            ' * The layer alone is not enough on purpose: it ships bundled' \
            ' * in the RZ/V2N AI SDK BSP, so keying off its presence would' \
            ' * flip the node on for every V2N/V2M image whether or not the' \
            ' * owner asked for an NPU.' \
            ' *' \
            ' * See e1m-v2n-drpai.dtsi in' \
            ' * meta-alp-sdk/recipes-kernel/linux/linux-renesas/.' \
            ' */' \
            > "${ALP_DTS_DST}/e1m-v2n-drpai.dtsi"
        # A shell redirect takes its mode from the builder's umask, unlike
        # the `install -m 0644` that lands every other file here; pin it so
        # both branches drop the same 0644 into the kernel DT source dir.
        chmod 0644 "${ALP_DTS_DST}/e1m-v2n-drpai.dtsi"
    fi
}

# Production kernel-config trims (linux-renesas is kernel-yocto based,
# so .cfg fragments in SRC_URI auto-merge).  Both grounded in the
# 2026-06-12 V2M101 boot-log audit; rationale inside each file.
SRC_URI:append = " \
    file://trim-unused-storage-net-fs.cfg \
    file://no-kernel-audit.cfg \
"

# Display stack: RK055HDMIPI4MA0 panel on Display 1 (DSI + PWM backlight + GPT
# + GD32-bridge GPIO for panel reset).
SRC_URI:append:e1m-v2n101 = " file://display.cfg"
SRC_URI:append:e1m-v2m101 = " file://display.cfg"
