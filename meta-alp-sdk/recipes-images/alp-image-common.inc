# SPDX-License-Identifier: Apache-2.0
#
# Shared core runtime for the Alp SDK images. This include is the headless
# base every image builds on; images differ by which OPT-IN feature groups
# (camera / display / ros -- see FEATURE_PACKAGES below) and which posture
# they add:
#   - alp-image-base: headless core only (production-capable minimal)
#   - alp-image-edge: base + all feature groups + debug-tweaks/bench tooling
#   - alp-image-prod: base + camera+display + production hardening (no ros)
# Everything common to all three lives here so the core payload has ONE source.

LICENSE = "Apache-2.0"

inherit core-image

IMAGE_FEATURES += "ssh-server-openssh"

# Block map next to the .wic.gz: scripts/provision_som.py write_rootfs writes and
# verifies only the mapped ranges (system_image_bmap bundle role) instead of the
# full ~7 GB image. wic.bmap pulls in bmaptool-native (oe-core). The V2N family
# is the one that ships a .wic.gz (vendor BSP IMAGE_FSTYPES).
IMAGE_FSTYPES:append:rzv2n-family = " wic.bmap"

# Core runtime payload (Linux side) -- present in EVERY Alp image: the SDK
# (which carries the portable <alp/inference.h> dispatcher + the silicon's
# NPU backend), the Mender OTA client, and the CA55-cluster watchdog policy.
# Camera, display, and the ROS 2 perception stack are OPT-IN feature groups
# (see FEATURE_PACKAGES below) so a base image stays headless and ROS-free.
IMAGE_INSTALL = " \
    packagegroup-core-boot         \
    alp-sdk                        \
    mender-client                  \
    openssh                        \
    alp-watchdog-policy            \
    alp-hostname                   \
"

# meta-rz-drpai vendor payload (DRP-AI3 NPU userspace runtime + kernel
# driver): kernel-module-mmngr (lib-tvm is deliberately NOT installed
# explicitly: it only served a legacy DRP-AI TVM v2.5 path, but libalp_sdk.so's
# tvm_runtime link still pulls it in via shlibs until the link fix lands). Unlike meta-rz-codecs /
# meta-rz-opencva (whose RDEPENDS payload lives in
# packagegroup-alp-camera.bb -- see that file), this is NOT
# camera/display-tied -- DRP-AI3 is the SoC's core NPU accelerator,
# reachable through <alp/inference.h> independent of any camera or
# display pipeline, so it belongs alongside alp-sdk in the core runtime
# every Alp image ships. This file's own header above already promised
# as much ("the Alp SDK + the silicon's NPU runtime ... machine-gated")
# -- base was already DOCUMENTED to ship the NPU runtime and didn't;
# that's the same #1176 silent-omission defect, in a second place.
# Wired here ONCE rather than repeated in alp-image-base.bb /
# alp-image-prod.bb / alp-image-edge.bb (the vendor payload was
# previously wired into alp-image-edge.bb only, silently omitted from
# base/prod).
#
# mmngr-user-module / mmngrbuf-user-module ride in via mera2-drpai-tvm's
# own RDEPENDS. The /dev/drpai* access rule (alp-drpai-udev) is NOT installed
# here: alp-sdk's PACKAGECONFIG[drpai] pulls it, so it lands only in images
# that carry the SDK DRP-AI backend. NOT ported from meta-rz-drpai's own vendor
# core-image-%.bbappend: its python3-modules/-pip/-wheel/.../-opencv/
# -pybind11 + sudo stack + libjpeg-turbo-dev (all serve the vendor's
# Python demos; the Alp path is C through <alp/inference.h>, and a -dev
# headers package has no business on a target rootfs regardless), its
# update_issue() ROOTFS_POSTPROCESS_COMMAND (case-matches only MACHINE
# rzv2n-evk/rzv2n-dev, so it would append empty BOARD:/LSI: lines to
# /etc/issue on an e1m-* machine), and its enable_sudo() (a NOPASSWD
# sudoers entry for the weston user -- a security-posture decision not
# to inherit silently, least of all into alp-image-base). This excludes
# meta-rz-drpai's OWN copy of `opencv`/`opencv-dev` specifically (the
# ones paired with its python3-opencv demo chain) -- it does NOT mean
# opencv is unavailable: meta-rz-opencva's `opencv` + `oca` ship
# separately, RDEPENDS of packagegroup-alp-camera.bb, on any
# alp-camera-enabled image (see that file). Two different vendor layers
# happen to want the same package name for different reasons; only the
# DRP-AI demo-chain route is excluded.
#
# Gated on the meta-rz-drpai LAYER being present (soft
# LAYERRECOMMENDS_alp-sdk dep -- its BBFILE_COLLECTIONS name, "rz-drpai",
# not its directory name; see conf/layer.conf) AND MACHINE_FEATURES
# containing "v2n" (set on all six V2N/V2M machine confs): a build can
# legitimately carry this layer in bblayers.conf while building a
# different MACHINE in the same tree (AEN, NX91 -- neither ever builds
# linux-renesas), and kernel-module-mmngr has no COMPATIBLE_MACHINE guard
# of its own to fall back on, so an unconditional RDEPENDS here would be
# a hard `do_rootfs` dependency with no guaranteed provider on those
# machines. Never gated on MACHINE alone (that would break parsing when
# the layer itself is dropped). Same two-gate reasoning as
# packagegroup-alp-camera.bb's ALP_RZ_CODECS_RDEPENDS -- see that file
# for the full explanation, including the drp-fw COMPATIBLE_MACHINE
# citation that is the concrete failure this closes. Settled by
# inspection only; unverified by an actual `bitbake` run.
ALP_RZ_DRPAI_INSTALL = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', \
    bb.utils.contains('MACHINE_FEATURES', 'v2n', 'kernel-module-mmngr', '', d), '', d)}"
ALP_RZ_DRPAI_INSTALL[vardepvalue] = "${ALP_RZ_DRPAI_INSTALL}"

IMAGE_INSTALL += " ${ALP_RZ_DRPAI_INSTALL}"

# On-module Murata LBEE5HY2FY-922 (Infineon CYW55513) Wi-Fi + BT: the
# out-of-tree cyw-fmac driver + firmware blobs (this layer, own
# recipes -- no vendor-layer gate needed) plus the standard userspace
# stack (wireless-regdb-static installs regulatory.db + regulatory.db.p7s
# under /lib/firmware for cfg80211 to load directly at modprobe -- kernel
# >= 4.15 only, which this 6.1 kernel clears; the bare wireless-regdb
# package is the legacy crda-daemon path (regulatory.bin under
# /usr/lib/crda/) and RCONFLICTS with -static, so picking the wrong one
# is not an additive mistake, iw for scan/connect from the shell,
# wpa_supplicant for WPA2/WPA3 association, bluez5 for hci0/
# bluetoothctl, and the BT kernel modules -- wifi-bt.cfg builds the BT stack
# as =m because a built-in hci_uart_bcm probes before rootfs and cannot
# load its firmware patch). Gated the same way ALP_RZ_DRPAI_INSTALL is -- on
# MACHINE_FEATURES containing "v2n" -- because every V2N/V2M SKU carries
# the same on-module part (see wifi_ble: in each
# metadata/e1m_modules/E1M-V2{N,M}10{1,2,3}.yaml), but a shared
# bblayers.conf building a non-RZ MACHINE (AEN, NX91) must not pull a
# dependency with no provider there.
ALP_V2N_WIFI_BT_INSTALL = "${@bb.utils.contains('MACHINE_FEATURES', 'v2n', \
    'cyw-fmac cyw-fmac-firmware wireless-regdb-static iw wpa-supplicant bluez5 kernel-module-bluetooth kernel-module-hci-uart kernel-module-btbcm', '', d)}"
ALP_V2N_WIFI_BT_INSTALL[vardepvalue] = "${ALP_V2N_WIFI_BT_INSTALL}"

IMAGE_INSTALL += " ${ALP_V2N_WIFI_BT_INSTALL}"

# Opt-in USB gadget (NCM + ACM) helper for the E1M-X-EVK USB 2.0 device port
# (docs/e1m-x-evk-usb-otg.md). Installed but not enabled at boot. Set
# ALP_ENABLE_USB_GADGET = "1" in local.conf to include it (this also merges
# the usb-gadget.cfg kernel fragment; default images are unchanged).
ALP_ENABLE_USB_GADGET ??= "0"
# Gated on the generic `usbgadget` MACHINE_FEATURES flag (set per machine conf);
# the same flag selects the kernel fragment in linux-renesas_%.bbappend.
# DX-M1 SoMs (`deepx-dxm1`) drive the E1M IO24 USB path-mux (driven_by: dxm1
# in the SoM pad_routes), so warn there.
IMAGE_INSTALL += "${@' alp-usb-gadget' if d.getVar('ALP_ENABLE_USB_GADGET') == '1' and bb.utils.contains('MACHINE_FEATURES', 'usbgadget', True, False, d) else ''}"
python () {
    if d.getVar('ALP_ENABLE_USB_GADGET') == '1' and bb.utils.contains('MACHINE_FEATURES', 'deepx-dxm1', True, False, d):
        bb.warn('ALP_ENABLE_USB_GADGET: the DX-M1 on this SoM drives the E1M IO24 USB path-mux and can switch the USB 2.0 port away from the SoC (metadata/boards/e1m-x-evk.yaml XEVK_PIN_USB_MUX_SEL).')
}

# SDK sysroot headers (populate_sdk) -- ports the vendor bbappends'
# TOOLCHAIN_TARGET_TASK:append mechanism, without which `bitbake
# alp-image-* -c populate_sdk` produces NO <linux/drpai.h> / <linux/drp.h>
# in the SDK sysroot, even though meta-alp-sdk/README.md documented those
# headers as appearing "at standard paths" once the layer is in
# bblayers.conf -- the same #1176 silent-omission shape, one level
# deeper (the SDK task graph, not the rootfs). README.md corrected in
# the same change.
#
# Quoted verbatim from the vendor sources (RZ/V2N AI SDK BSP v6.30):
#   meta-rz-drpai/recipes-core/images/core-image-%.bbappend:21-22
#     TOOLCHAIN_TARGET_TASK:append = " drpai ... " (drpai's sibling
#     python3-* SDK entries are the same vendor Python-demo tooling
#     excluded above and are NOT ported)
#   meta-rz-codecs/recipes-core/images/core-image-%.bbappend:3
#     TOOLCHAIN_TARGET_TASK:append = " drp "
#   meta-rz-opencva/recipes-core/images/core-image-%.bbappend:3
#     TOOLCHAIN_TARGET_TASK:append = " drp "
# "drpai" -> meta-rz-drpai's drpai_1.4.0.bb, a header-only recipe
# installing <linux/drpai.h> (the DRP-AI3 ioctls). "drp" -> the drp_1.3.0.bb
# recipe (present, byte-identical, in BOTH meta-rz-codecs and
# meta-rz-opencva) installing <linux/drp.h> (the DRP compute-unit ioctls
# drp-fw / oca build against). Each is gated on its own layer's
# collection, same as ALP_RZ_DRPAI_INSTALL above; appending "drp" twice
# when both layers are present is harmless (TOOLCHAIN_TARGET_TASK is a
# package-name list, and a repeated name is a no-op), so no dedup logic
# is added for it.
ALP_RZ_DRPAI_SDK_TASK = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-drpai', 'drpai', '', d)}"
ALP_RZ_DRPAI_SDK_TASK[vardepvalue] = "${ALP_RZ_DRPAI_SDK_TASK}"

ALP_RZ_CODECS_SDK_TASK = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'meta-rz-codecs', 'drp', '', d)}"
ALP_RZ_CODECS_SDK_TASK[vardepvalue] = "${ALP_RZ_CODECS_SDK_TASK}"

ALP_RZ_OPENCVA_SDK_TASK = "${@bb.utils.contains('BBFILE_COLLECTIONS', 'rz-opencva', 'drp', '', d)}"
ALP_RZ_OPENCVA_SDK_TASK[vardepvalue] = "${ALP_RZ_OPENCVA_SDK_TASK}"

TOOLCHAIN_TARGET_TASK:append = " ${ALP_RZ_DRPAI_SDK_TASK} ${ALP_RZ_CODECS_SDK_TASK} ${ALP_RZ_OPENCVA_SDK_TASK}"

# Composable feature groups -- the customer-facing composition surface. Each
# maps to an IMAGE_FEATURES token so an image pulls only what it needs:
#   IMAGE_FEATURES += "alp-camera"   -> libcamera + GStreamer
#   IMAGE_FEATURES += "alp-display"  -> Weston/Wayland compositor
#   IMAGE_FEATURES += "alp-ros"      -> ROS 2 Humble + alp-perception
# alp-image-base ships none of them; alp-image-edge enables all three
# (+ debug tooling); alp-image-prod enables camera+display (vision
# appliance) but NOT ros. A new capability lands as another packagegroup
# here -- never an edit to the image recipes.
FEATURE_PACKAGES_alp-camera  = "packagegroup-alp-camera"
FEATURE_PACKAGES_alp-display = "packagegroup-alp-display"
FEATURE_PACKAGES_alp-ros     = "packagegroup-alp-ros"

# meta-rz-codecs / meta-rz-opencva's RDEPENDS payload (drp-fw / opencv
# oca) is NOT wired here -- per this section's own rule ("a new
# capability lands as another packagegroup here -- never an edit to the
# image recipes"), it rides packagegroup-alp-camera.bb's RDEPENDS,
# gated the same way ALP_RZ_DRPAI_INSTALL is gated above. See that file
# for the full #1176-follow-up rationale.

# Deterministic boot plumbing (2026-06-12 boot-log audit):
#   - alp-network-defaults: pins the wired-DHCP networkd story in this
#     layer (overrides oe-core systemd-conf's 80-wired.network while
#     keeping its nfsroot/ip= guards -- see the recipe header).
#   - rng-tools + rng-tools-service: rngd (jitterentropy source)
#     credits the entropy pool early so first-boot key generation
#     (ssh host keys, the Mender device identity) doesn't ride on
#     incidental jitter alone. BOTH packages are required: oe-core
#     splits the systemd unit + preset into rng-tools-service and
#     neither package pulls the other -- rng-tools alone installs a
#     daemon that never starts. (rngd feeding the kernel pool is the
#     mechanism src/yocto/security_yocto.c's TRNG-path note assumes;
#     this SoC has no /dev/hwrng yet, so the source is CPU jitter.)
IMAGE_INSTALL += " \
    alp-network-defaults           \
    rng-tools                      \
    rng-tools-service              \
"

# RZ/V2N family: no rngd. rngd 6.16 finds no usable source on this SoC --
# hwrng (no /dev/hwrng), rndr and jitter all fail initialisation -- so
# rng-tools.service failed on every boot after ~16 s of CPU and credited
# nothing (E1M-V2M103, 2026-09-29). The kernel CRNG initialises on its own
# (`random: crng init done` at ~6.9 s, CONFIG_CRYPTO_JITTERENTROPY=y and
# CONFIG_RANDOM_TRUST_CPU=y), and getrandom() blocks until then, so
# first-boot key generation is not starved. Drop this once the SoC has a
# hwrng driver.
IMAGE_INSTALL:remove:rzv2n-family = "rng-tools rng-tools-service"

# scripts/provision_som.py runs on the unit's own Linux: write_emmc_boot
# reads and sets the eMMC boot config (EXT_CSD[177]/[179]) with `mmc`.
# busybox has no equivalent, and no image carried it before (2026-09-28,
# E1M-V2M103 2026W38-0001: "mmc: command not found").
IMAGE_INSTALL += " mmc-utils"

# fw_printenv/fw_setenv + /etc/fw_env.config for the persistent U-Boot
# environment (eMMC boot partition 2, redundant pair; u-boot patch 0016 +
# uboot-env-emmc.cfg). Groundwork for the OTA design: only the OTA
# variables persist (U-Boot write allowlist); nothing reads a boot-slot
# variable yet. Mender builds get their own fw_env.config from
# mender-full, so conf/distro/include/mender.inc removes this package.
IMAGE_INSTALL:append:rzv2n-family = " alp-uboot-env"

# Assert the network story: the vendor build template's
# EXTRA_IMAGE_FEATURES (local.conf) includes tools-testapps, whose
# packagegroup drags in connman -- a second DHCP manager fighting
# networkd on end0/end1 (address/resolv.conf flapping, and a hazard
# for benches reached over SSH at a fixed address). Both images are
# networkd-only.
IMAGE_FEATURES:remove = "tools-testapps"

# DEEPX runtime on V2M variants: the machine confs own this (see
# conf/machine/include/e1m-v2m-deepx.inc), gated on
# ALP_ENABLE_DEEPX_DXM1, which defaults on whenever DEEPX's own
# meta-deepx-m1 layer (dx-driver/dx-rt) is in bblayers.conf. No
# unconditional append here: it would make every V2M image require
# meta-deepx-m1 in bblayers.conf.

IMAGE_LINGUAS = "en-us"

# 4 GB rootfs target -- headroom for the opt-in feature groups (ROS 2 +
# GStreamer + DEEPX assets, when enabled) plus customer payload. A headless
# alp-image-base uses a fraction of this; the ceiling is sized for the
# everything-on alp-image-edge so all variants share one Mender slot size.
IMAGE_ROOTFS_EXTRA_SPACE = "1048576"
IMAGE_ROOTFS_SIZE = "4194304"

# U-Boot's vendor sd2load/emmcload env (bootcmd_check) still loads the stock
# EVK dtb name boot/r9a09g056n44-dev.dtb before the ALP boot command loads the
# real board dtb (boot/${CONFIG_ALP_E1M_FDTFILE}) over it at the same address.
# No ALP MACHINE builds that name, so the vendor load printed
# "Failed to load 'boot/r9a09g056n44-dev.dtb'" on every boot. Link the stock
# name to the board dtb so the vendor load succeeds harmlessly; the ALP load
# that follows still decides which dtb boots. The vendor env text is patched
# by several vendor layers, so changing U-Boot's side would be fragile. The
# link is relative, so a swap of the real dtb (provisioning's dxuart2 step)
# is followed automatically. V2N/V2M (MACHINE_FEATURES "v2n") only.
ALP_VENDOR_FDT_NAME = "r9a09g056n44-dev.dtb"
ALP_BOARD_FDT_NAME  = "${@os.path.basename((d.getVar('KERNEL_DEVICETREE') or '').split()[0]) if (d.getVar('KERNEL_DEVICETREE') or '').split() else ''}"

alp_link_vendor_fdt_name() {
	# Either condition makes an unbootable image: CONFIG_BOOTCOMMAND refuses
	# to boot when boot/${CONFIG_ALP_E1M_FDTFILE} does not load.
	if [ -z "${ALP_BOARD_FDT_NAME}" ]; then
		bbfatal "no KERNEL_DEVICETREE for ${MACHINE}; U-Boot would not find a board dtb"
	elif [ ! -e "${IMAGE_ROOTFS}/boot/${ALP_BOARD_FDT_NAME}" ]; then
		bbfatal "/boot/${ALP_BOARD_FDT_NAME} is not in the rootfs; U-Boot would not find a board dtb"
	else
		ln -sf "${ALP_BOARD_FDT_NAME}" "${IMAGE_ROOTFS}/boot/${ALP_VENDOR_FDT_NAME}"
	fi
}
ROOTFS_POSTPROCESS_COMMAND:append = "${@' alp_link_vendor_fdt_name;' if 'v2n' in (d.getVar('MACHINE_FEATURES') or '').split() else ''}"
