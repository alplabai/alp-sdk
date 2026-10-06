# meta-alp-sdk: ALP E1M board device trees for the Renesas RZ/V2N SoM
# family, layered the way SoM vendors ship BSPs (SoC dtsi -> SoM dtsi ->
# carrier dtsi -> per-board dts -> named dtb), NOT as a patch pile against
# the EVK reference dts.
#
#   e1m-v2n-som.dtsi    on-module V2N: dual GbE PHYs, eMMC, xSPI NOR,
#                       DRP-AI reserved memory, core rails.
#   e1m-v2n-ownership.dtsi  GENERATED (scripts/gen_linux_ownership_dt.py): the
#                       per-product core-ownership nodes (UART0/1, SPI0, CAN-FD).
#   e1m-v2n-drpai.dtsi  the &drpai0 enable that claims that reserved memory.
#                       Installed ONLY when meta-rz-drpai is in bblayers
#                       (it creates the label); stubbed out otherwise --
#                       see the ALP_DRPAI_DT_ENABLE block below.
#   e1m-v2m-deepx.dtsi  V2M delta: DEEPX DXM1 NPU on PCIe + the on-module
#                       lane mux + NPU reset release (gpio-hogs).
#   e1m-x-evk.dtsi      E1M-X-EVK carrier: eth/i2c/usb/console enables,
#                       USB-OVC hog, DSI display, TAS2563 audio. (CAN is TODO;
#                       cameras are opt-in, see camera-csi.cfg.)
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
    file://e1m-v2n-ownership.dtsi \
    file://e1m-x-evk.dtsi \
    file://e1m-v2m-deepx.dtsi \
    file://e1m-v2n101-x-evk.dts \
    file://e1m-v2m101-x-evk.dts \
    file://0001-clk-renesas-rzv2h-cpg-cm33-owned-clocks.patch \
    file://0002-drm-renesas-rzg2l-mipi-dsi-pm_runtime-guard-host-tra.patch \
    file://0003-usb-ohci-platform-add-spurious-oc-DT-property.patch \
    file://0004-drm-panel-add-himax-hx8394-with-rocktech-rk055hdmipi.patch \
    file://0005-gpio-add-gd32-bridge-expander-driver.patch \
    file://0006-input-goodix-fall-back-to-polling-without-an-irq.patch \
    file://0007-mmc-renesas_sdhi-pm_runtime-guard-the-vqmmc-regulato.patch \
    file://0010-mmc-renesas_sdhi-bounce-multi-segment-requests-in-internal-dmac.patch \
    file://0011-irqchip-renesas-rzv2h-mask-the-ICU-error-sources-the-handler-cannot-ack.patch \
    file://0012-uio-pdrv-genirq-default-of_id-to-generic-uio.patch \
    file://0015-gpiolib-sysfs-reject-export-of-a-number-in-a-chipless-gpio_device.patch \
    file://0016-media-rzg2l-cru-add-Y10-Y8-greyscale-formats.patch \
    file://0017-media-rzg2l-csi2-honour-lane-polarities-via-SWAPCTL.patch \
    file://0020-clk-renesas-r9a09g056-add-the-PDM-module-clocks-and-resets.patch \
    file://0021-gpio-gd32-bridge-add-cam-en-ldo-lines-and-make-can-stby-requestable.patch \
    file://0022-gpio-gd32-bridge-i2c3-proxy-adapter-and-polled-irqchip.patch \
    file://uio.cfg \
"

# 0021..0022 (GD32 bridge, bridge protocol 0.17; both patch gpio-gd32-bridge.c
# that 0005 adds, so they apply strictly after it and in this order):
#   0021  CAM_EN_LDO0..3 as gpio lines 24..27 (gated on minor >= 17) and
#         can-stby (line 20) made requestable for a phy-can-transceiver
#         standby-gpios.
#   0022  an i2c_adapter for E1M-X I2C3 (GD32 PC8/PC9) from the bridge node's
#         "i2c" child (label e1m_x_i2c3), checked lazily per transfer against
#         minor >= 17, plus a polled irqchip on the bridge gpiochip (10 ms
#         GPIO_READ while any line is unmasked).
# The kernel option they need (GPIOLIB_IRQCHIP) is selected by the
# GPIO_GD32_BRIDGE Kconfig entry.

# 0020 (PDM clocks, audit MM-02/MM-X2): the V2N CPG driver had no PDM0/PDM1
# module clocks or resets, so no pdm node could bind.  The patch adds them
# with V2N parents from the RZ/V2N hardware manual (PCLK = PLLCM33 gear / 2,
# CCLK = QEXTAL / 5 = 4.8 MHz).  No devicetree node uses them yet.
#
# pcie-ep-trim.cfg (audit PCIE-5): drops the PCIe endpoint-mode and test
# options the Renesas defconfig enables for the EVK; E1M-V2M is root-complex
# only.
#
# 0016 (CRU greyscale, #2612): rzg2l-csi2 had no Y10/Y8 entry, so a mono
# sensor's Y10_1X10 (OV9281 via ov9282) read back as UYVY8_1X16 on the
# csi20 pad and STREAMON failed -EPIPE.  The patch adds Y10_1X10 (-> CR10,
# RAW10) and Y8_1X8 (-> GREY, RAW8) to the CSI-2 and CRU tables; no other
# sensor's behaviour changes, so it applies unconditionally.
#
# 0017 (CSI-2 lane polarity, #2612): the RZ/V2H-family D-PHY swaps a lane's
# DP/DN pair through CRUm_SWAPCTL, but the driver always wrote 0 and ignored
# the DT lane-polarities property.  On E1M-V2M103 + X-EVK J5 the CSI0 pairs
# arrive swapped (ErrControl on the data lanes, no packets) until SWAPCTL =
# 0x30.  The patch programs it from lane-polarities (all data lanes or none);
# the cam0 dtsi fragments set <1 1 1> on the csi20 endpoint.  Applied
# unconditionally: with no lane-polarities the register is still written 0.
#
# 0012 (UIO default match, #2374): uio_pdrv_genirq binds no DT node until
# of_id is set, and the stored U-Boot bootargs cannot be relied on to carry
# uio_pdrv_genirq.of_id=generic-uio; the patch defaults it to "generic-uio".
#
# 0010 (SDHI internal-DMAC bounce buffer, #2357): the DMAC takes one
# contiguous buffer per request and the RZ/V2N SDHI has no IOMMU, so every
# page-cache write reached the card as a separate 4 KiB command (microSD
# ~2.7 MB/s, and SDR104 writes hung on "Card stuck being busy!"). The patch
# copies multi-segment requests through a 256 KiB coherent buffer per host.
#
# 0011 (ICU error mask, #2355): the shared CA55 ICU error line is serviced
# only for GPT overflow bits, but group 0 resets fully unmasked; once the
# Cortex-M33 runs, group 0 bit 0 asserts, nobody acknowledges it, and the
# line storms ("irq 14: nobody cared") until genirq disables it. The patch
# unmasks only the GPT overflow bits the handler services.  It also logs and
# clears the ICU bus-error factors (ICU_BEISR0-3 / ICU_BECLR0-3) at probe and
# names MCPU_LOCKUP, so a masked source is not a silent one.
# The mask is written BEFORE the line is requested: requesting enables the
# line, and a source already asserted at probe storms it inside the request.

# AMP clock ownership: peripherals that belong to the Cortex-M33 system
# manager (RSCI7 = the GD32 supervisor SPI link, always; any assignable block
# a product hands to the M33).  Without this patch, Linux's
# clk_disable_unused turns their module clocks off AND asserts the coupled
# CPG BUS_MSTOP bits (the rzv2h-cpg driver ties the two together), which
# bus-faults the CM33 mid-operation ~15 s into every boot.  The patch makes
# the CPG driver keep every module clock named in the CPG node's
# `renesas,cm33-owned-clocks` property critical, so both gates stay held for
# the remote core.  The list is not hand-written: it is generated into
# e1m-v2n-ownership.dtsi (scripts/gen_linux_ownership_dt.py) from the SoM
# ownership metadata -- RSCI7 for the GD32 link, plus the clocks of each
# assignable instance owned by the M33.  The earlier hard-coded
# DEF_MOD_CRITICAL form of this fix was silicon-validated 2026-06-03 (two
# cold cycles + warm reboot, link autonomous from ~2 s after power-on); this
# DT-driven form applies to the BSP kernel (6717c06) but is NOT yet
# bench-validated -- re-run that cold-cycle check on the first build.
#
# RIIC8 (BRD_I2C) is not listed: the maintainer decision that
# Cortex-A55/Linux is RIIC8's sole master (metadata/e1m_modules/v2n/
# core-ownership.yaml) makes Linux the real consumer -- its own
# clk_disable_unused correctly leaves riic_8_ckm alone.

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

# 0007 (SDHI vqmmc regulator read while runtime-suspended): the vqmmc
# regulator this recipe's &sdhi2 WLAN node registers (see e1m-v2n-som.dtsi's
# sdhi2_vqmmc) reads CTL_SD_STATUS directly in is_enabled()/get_voltage(),
# with no pm_runtime claim on the SDHI host.  regulator-always-on only
# short-circuits regulator_late_cleanup()'s OWN call to is_enabled() (its
# `if (c->always_on) return 0;` early-return); a later sysfs/debugfs
# regulator read still hits the raw register access and can take a
# synchronous external abort while the controller is clock-gated.  0007
# wraps both ops in pm_runtime_resume_and_get()/pm_runtime_put().

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

# ...and the node defaults ON with the layer.  An earlier revision kept it
# off unless ALP_ENABLE_DRPAI = "1" was set by hand, so every shipped V2N/V2M
# image carried the DRP-AI3 driver, its reserved arena and the vendor
# runtime, yet no /dev/drpai0: the NPU was unreachable on the product.
# DRP-AI3 is on-die on every V2N/V2M SKU, so a V2x image that has the
# vendor layer now gets the node; ALP_ENABLE_DRPAI = "0" in local.conf
# opts out.  The six V2N/V2M machine confs declare the same default, and
# the `??=` here is only the fallback for a consumer that uses this
# bbappend without one of them.  Turning the SDK backend on
# (PACKAGECONFIG "drpai") stays a separate switch: it needs a RUHMI
# checkout, and alp-sdk_0.6.bb auto-enables it only when one is configured.
ALP_ENABLE_DRPAI ??= "${@'1' if 'rz-drpai' in (d.getVar('BBFILE_COLLECTIONS') or '').split() else '0'}"
ALP_DRPAI_DT_ENABLE = "${@'1' if (d.getVar('ALP_DRPAI_LAYER') == '1' and d.getVar('ALP_ENABLE_DRPAI') == '1') else '0'}"
ALP_DRPAI_DT_ENABLE[vardepvalue] = "${ALP_DRPAI_DT_ENABLE}"
SRC_URI += "${@' file://e1m-v2n-drpai.dtsi' if d.getVar('ALP_DRPAI_DT_ENABLE') == '1' else ''}"

# DRP1 (OpenCVA + hardware codec) overlay -- CONDITIONAL on meta-rz-opencva
# or meta-rz-codecs, whichever supplies the `drp1` label (each ships the
# 0001-add-drp-property-to-devicetree*.patch that creates it).  Same
# stub-or-real shape as ALP_DRPAI_DT_ENABLE above; the node is on-die on
# every V2N/V2M SKU, so the layer is the only axis.
ALP_DRP1_DT_ENABLE = "${@'1' if ('rz-opencva' in (d.getVar('BBFILE_COLLECTIONS') or '').split() or 'meta-rz-codecs' in (d.getVar('BBFILE_COLLECTIONS') or '').split()) else '0'}"
ALP_DRP1_DT_ENABLE[vardepvalue] = "${ALP_DRP1_DT_ENABLE}"
SRC_URI += "${@' file://e1m-v2n-drp1.dtsi' if d.getVar('ALP_DRP1_DT_ENABLE') == '1' else ''}"

# Per-project core ownership (#2660).  The committed e1m-v2n-ownership.dtsi is
# the SoM-DEFAULT ownership (kept in sync by the generated-files gate); do_configure
# replaces it with the fragment of the project being built, rendered from the
# `ownership:` that the orchestrator's system-manifest.yaml carries (the same
# resolved map the CM33 overlay uses).  Two cases:
#   1. manifest present -> render the project fragment from it.
#   2. no manifest      -> bbwarn and keep the committed SoM-default fragment, so
#                          a generic SoM image still builds.  The default fragment
#                          carries the renesas,cm33-owned-clocks hold that keeps
#                          the CM33's RSCI7 clocks on, so it is never absent.
# The manifest path variable and default match alp-dts-reservations.  Freshness
# is the manifest's: it is only as current as the last `tan build`.
ALP_SYSTEM_MANIFEST_PATH ??= "${TOPDIR}/../alp-sdk/build/system-manifest.yaml"
ALP_OWN_SDK := "${THISDIR}/../../.."
# Everything the render reads, so editing any of it re-runs do_configure.
do_configure[file-checksums] += "${ALP_SYSTEM_MANIFEST_PATH}:${@os.path.exists(d.getVar('ALP_SYSTEM_MANIFEST_PATH'))} \
    ${ALP_OWN_SDK}/scripts/gen_linux_ownership_dt.py:True \
    ${ALP_OWN_SDK}/scripts/alp_orchestrate/linux_ownership.py:True \
    ${ALP_OWN_SDK}/scripts/alp_orchestrate/ownership.py:True \
    ${ALP_OWN_SDK}/metadata/e1m_modules/v2n/core-ownership.yaml:True \
    ${ALP_OWN_SDK}/metadata/e1m_modules/v2n/supervisor-links.yaml:True \
    ${ALP_OWN_SDK}/metadata/socs/renesas/rzv2n/n44.json:True"

# Drop the ALP board dts + dtsi into the kernel DT source dir so they
# compile next to the upstream Renesas dts (the board dts #include the
# SoC r9a09g056.dtsi and these dtsi by relative path).
ALP_DTS_DST = "${S}/arch/arm64/boot/dts/renesas"
do_configure:prepend() {
    install -m 0644 \
        "${WORKDIR}/e1m-v2n-som.dtsi" \
        "${WORKDIR}/e1m-v2n-ownership.dtsi" \
        "${WORKDIR}/e1m-x-evk.dtsi" \
        "${WORKDIR}/e1m-v2m-deepx.dtsi" \
        "${WORKDIR}/e1m-v2n101-x-evk.dts" \
        "${WORKDIR}/e1m-v2m101-x-evk.dts" \
        "${ALP_DTS_DST}/"

    # Per-project ownership (see the two cases above).  A manifest renders
    # over the SoM-default fragment installed above, and every node it names
    # must exist in THIS kernel's SoC dtsi.
    ALP_OWN_GEN="${ALP_OWN_SDK}/scripts/gen_linux_ownership_dt.py"
    ALP_OWN_M="${ALP_SYSTEM_MANIFEST_PATH}"
    if [ -f "${ALP_OWN_M}" ]; then
        [ -f "${ALP_OWN_GEN}" ] && python3 -c 'import yaml' 2>/dev/null \
            || bbfatal "gen_linux_ownership_dt.py or PyYAML unavailable: cannot render the ownership fragment for ${ALP_OWN_M}"
        python3 "${ALP_OWN_GEN}" --manifest "${ALP_OWN_M}" \
            --output "${ALP_DTS_DST}/e1m-v2n-ownership.dtsi" \
            --vendor-dtsi "${S}/arch/arm64/boot/dts/renesas/r9a09g056.dtsi" \
            || bbfatal "per-project ownership fragment for ${ALP_OWN_M} is invalid or names a node r9a09g056.dtsi lacks"
        bbnote "per-project ownership fragment rendered from ${ALP_OWN_M}"
    else
        bbwarn "no system-manifest at '${ALP_OWN_M}': using the committed SoM-default ownership fragment (pass ALP_SYSTEM_MANIFEST_PATH for a project build)"
        # The committed fragment must still match the metadata and only name
        # nodes THIS kernel's SoC dtsi defines; --check never writes.
        if [ -f "${ALP_OWN_GEN}" ] && python3 -c 'import yaml' 2>/dev/null; then
            python3 "${ALP_OWN_GEN}" --check                 --vendor-dtsi "${S}/arch/arm64/boot/dts/renesas/r9a09g056.dtsi"                 || bbfatal "e1m-v2n-ownership.dtsi is stale or names a node r9a09g056.dtsi lacks"
        else
            bbwarn "gen_linux_ownership_dt.py or PyYAML unavailable: ownership fragment not verified against r9a09g056.dtsi"
        fi
    fi

    # Opt-in CAM0 sources (#1149): the wrapper dts + fragment must sit next
    # to the board dts or the cam0 dtb has no rule to build.
    if [ -n "${ALP_CAM0_SENSOR}" ]; then
        install -m 0644 "${WORKDIR}/e1m-x-evk-cam0-${ALP_CAM0_SENSOR}.dtsi" \
            "${ALP_DTS_DST}/e1m-x-evk-cam0-sensor.dtsi"
        install -m 0644 \
            "${WORKDIR}/e1m-v2n101-x-evk-cam0.dts" \
            "${WORKDIR}/e1m-v2m101-x-evk-cam0.dts" \
            "${ALP_DTS_DST}/"
    fi

    # DRP1: branch on the variable, same reasoning as the DRPAI branch below.
    if [ "${ALP_DRP1_DT_ENABLE}" = "1" ]; then
        install -m 0644 "${WORKDIR}/e1m-v2n-drp1.dtsi" "${ALP_DTS_DST}/"
    else
        printf '%s\n' \
            '/* DRP1 (OpenCVA + codec) node not claimed in this build.' \
            ' * Needs meta-rz-opencva or meta-rz-codecs in bblayers.conf: it' \
            ' * supplies the &drp1 label.  See e1m-v2n-drp1.dtsi in' \
            ' * meta-alp-sdk/recipes-kernel/linux/linux-renesas/. */' \
            > "${ALP_DTS_DST}/e1m-v2n-drp1.dtsi"
        chmod 0644 "${ALP_DTS_DST}/e1m-v2n-drp1.dtsi"
    fi

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
    file://pcie-ep-trim.cfg \
"

# On-module RTC (all six V2N-family SKUs carry the same RV-3028-C7 --
# see rtc_external: in each metadata/e1m_modules/E1M-V2{N,M}10{1,2,3}.yaml).
# Unconditional like the two trims above, not per-machine like
# display.cfg: this is a SoM-level fact, not a carrier one.
SRC_URI:append = " file://rv3028-rtc.cfg"

# On-module Murata LBEE5HY2FY-922 (Infineon CYW55513) Wi-Fi + BT -- all
# six V2N-family SKUs carry the same module (see wifi_ble: in each
# metadata/e1m_modules/E1M-V2{N,M}10{1,2,3}.yaml). Unconditional like
# rv3028-rtc.cfg above: a SoM-level fact, not a per-machine one. See
# e1m-v2n-som.dtsi for the &sdhi2 WLAN node + &sci4 BT node, and
# meta-alp-sdk/recipes-kernel/cyw-fmac{,-firmware}/ for the out-of-tree
# driver + blobs this fragment's CFG80211=m / BRCMFMAC=n pairs with.
SRC_URI:append = " file://wifi-bt.cfg"

# Display stack: RK055HDMIPI4MA0 panel on Display 1 (DSI + PWM backlight + GPT
# + GD32-bridge GPIO for panel reset).
SRC_URI:append:e1m-v2n101 = " file://display.cfg"
SRC_URI:append:e1m-v2m101 = " file://display.cfg"

# Audio: TAS2563 smart-amp pair on the E1M-X-EVK carrier (see e1m-x-evk.dtsi's
# header comment + &i2c0's tas2563_left/tas2563_right nodes). Per-carrier like
# display.cfg above, not unconditional: it is the E1M-X-EVK's TAS2563 pair,
# not a SoM-level fact.
# Keyed on e1m-v2n101 ONLY: every V2N-family machine, the V2M ones
# included, carries that override (conf/machine/e1m-v2m10*-a55.conf), so a
# second :e1m-v2m101 append would add the patch twice and do_patch fails.
SRC_URI:append:e1m-v2n101 = " file://tas2563-audio.cfg file://0009-ASoC-tas2562-reset-the-amplifier-at-probe.patch file://0014-ASoC-rsnd-let-SSI2-share-SSI1-SCK-WS-on-RZ-V2N.patch"

# USB device (gadget) mode on the E1M-X-EVK USB 2.0 port (docs/e1m-x-evk-usb-otg.md).
# OPT-IN, BENCH-UNVERIFIED: usb-gadget.cfg builds the Renesas USBHS driver
# in, which binds the otg &hsusb node, so it is merged ONLY when
# ALP_ENABLE_USB_GADGET = "1" (machines with the `usbgadget` MACHINE_FEATURES flag, the same gate as the image install).
SRC_URI:append = "${@' file://usb-gadget.cfg' if d.getVar('ALP_ENABLE_USB_GADGET') == '1' and bb.utils.contains('MACHINE_FEATURES', 'usbgadget', True, False, d) else ''}"

# Camera (#1149): OPT-IN IMX219 on the E1M-X-EVK CAM0 connector ->
# CSI-2 receiver -> CRU0.  BENCH-UNVERIFIED.  Off by default: the shipped
# dtb does not change.  Set ALP_ENABLE_CAM0_IMX219 = "1" in local.conf to
# ALSO build renesas/e1m-v2{n,m}101-x-evk-cam0.dtb and merge
# camera-csi.cfg; the bootloader `fdtfile` must then name that dtb (the
# default dtb stays in KERNEL_DEVICETREE as the fallback).  Placeholder
# sensor + assumed CSI/CRU labels: see e1m-x-evk-cam0-imx219.dtsi and
# docs/v2n-camera-csi.md.
ALP_ENABLE_CAM0_IMX219 ??= "0"
# Camera (#2612): OPT-IN OV9281 (mono, RPi-style module) on the same CAM0
# connector (J5).  Mutually exclusive with the IMX219 switch.  Adds
# camera-csi.cfg (CONFIG_VIDEO_OV9282) and the same cam0 dtb; see
# e1m-x-evk-cam0-ov9281.dtsi and docs/v2n-camera-csi.md.
ALP_ENABLE_CAM0_OV9281 ??= "0"
python () {
    imx219 = d.getVar('ALP_ENABLE_CAM0_IMX219') == '1'
    ov9281 = d.getVar('ALP_ENABLE_CAM0_OV9281') == '1'
    if imx219 and ov9281:
        bb.fatal("ALP_ENABLE_CAM0_IMX219 and ALP_ENABLE_CAM0_OV9281 are mutually exclusive: both are CAM0 sensors")
    d.setVar('ALP_CAM0_SENSOR', 'imx219' if imx219 else 'ov9281' if ov9281 else '')
}
ALP_CAM0_DTB = "${@'e1m-v2m101-x-evk-cam0' if 'v2m' in d.getVar('MACHINE') else 'e1m-v2n101-x-evk-cam0'}"
KERNEL_DEVICETREE:append = "${@' renesas/' + d.getVar('ALP_CAM0_DTB') + '.dtb' if d.getVar('ALP_CAM0_SENSOR') else ''}"
SRC_URI += "${@' file://camera-csi.cfg file://e1m-x-evk-cam0-' + d.getVar('ALP_CAM0_SENSOR') + '.dtsi file://e1m-v2n101-x-evk-cam0.dts file://e1m-v2m101-x-evk-cam0.dts' if d.getVar('ALP_CAM0_SENSOR') else ''}"
