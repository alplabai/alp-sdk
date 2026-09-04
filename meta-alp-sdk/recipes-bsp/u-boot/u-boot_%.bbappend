# meta-alp-sdk: bring up the on-module DEEPX DX-M1 in U-Boot, before the
# A55 kernel's PCIe root complex probes.
#
# WHY: the V2N-M1 SoM wires the DX-M1 behind two passive PI3DBS12212A PCIe
# muxes (PD on Renesas P80, SEL on P95) and an active-low reset M1_RESET
# (PA6). The A55 Linux PCIe RC (rzg3s-pcie-host) trains the link in its
# builtin probe very early (~1.7s) and the driver has no gpio/reset hook,
# so Linux gpio-hogs apply too late -- the RC trains into a powered-down
# mux and fails with -ETIMEDOUT. Boot is A55-only here, so the only layer
# that runs before the RC is U-Boot.
#
# WHAT: board_late_init() (board/renesas/rzv2n-dev/rzv2n-dev.c) reads the
# on-module hardware-info manifest from the RIIC0 24C128 EEPROM (0x50),
# validates magic + schema_version + CRC32 (EEPROM-MANIFEST-SPEC.md), and
# ONLY when the manifest reports family "v2n-m1" sets, before bootcmd:
#     P80 = low   -> enable the PI3DBS12212A muxes (PD active-low)
#     P95 = low   -> route PCIe to the DEEPX path (path_0)
#     PA6 = high  -> release M1_RESET (active-low)
# So the same U-Boot is safe on a non-DEEPX V2N SoM (no manifest match ->
# DEEPX path left untouched). RIIC0 (P30/P31) pinmux is added to s_init and
# the i2c0 node enabled in rzv2n-dev.dts. (Pair with the kernel dtb's pcie
# num-lanes=2.) DEEPX rails are always-on (current SoM rev's standalone
# buck), so no rail sequencing is needed.
#
# Targets the renesas-u-boot-cip SRCREV this BSP pins for rzv2n-family
# (2024.07, bcf29d98); applies on top of meta-renesas's rzv2n-dev PMIC-I2C
# removal patch.

FILESEXTRAPATHS:prepend := "${THISDIR}/${PN}:"

SRC_URI:append:rzv2n-family = " \
    file://0001-rzv2n-dev-EEPROM-gated-DEEPX-DX-M1-PCIe-bring-up.patch \
    file://0002-rzv2n-dev-ALP-E1M-production-boot.patch \
    file://no-dirty-version.cfg \
"

# no-dirty-version.cfg: the vendor defconfig's CONFIG_LOCALVERSION_AUTO
# appends `git describe --dirty` -- always "-dirty" here because the
# patches above are git-applied into the tree -- leaking a "-dirty"
# flag + the upstream SHA into the boot banner. The fragment disables
# the auto version and pins an ALP localversion. Merged into the
# resolved defconfig by u-boot-configure.inc (find_cfgs() + merge_config.sh
# pick up any *.cfg in SRC_URI) -- the same path prod-boot.cfg uses.

# 0002 (production boot): four build-gated ALP additions to the same
# rzv2n-dev board files.
#   CONFIG_ALP_E1M_EMMC_1V8 (default y in the patched defconfig):
#     drive eMMC_V_SEL (PA0) high in board_init, BEFORE the first MMC
#     access (initr_env loads the environment from eMMC right after
#     board_init in init_sequence_r) -- the E1M SoM's eMMC/SD0 IO rail
#     powers up at 3.3 V and eMMC has no live VccQ-switch handshake,
#     so the whole chain must run the card at 1.8 V from the start.
#     Pairs with the Linux-side PA0 gpio-hog (e1m-v2n-som.dtsi).
#   CONFIG_ALP_PROD_BOOT (default n, DEV-target only): production
#     kernel cmdline (console= pinned + quiet, earlycon dropped). The
#     dev cmdline keeps earlycon but gains console=ttySC0,115200,
#     which stops the kernel replaying the early log across the
#     console handover. The cmdline is rebuilt at CONFIG_BOOTCOMMAND
#     (patch-safe vs the build-varying env block).
#   CONFIG_ALP_FDT_FILE + CONFIG_ALP_SD_ROOT (per-MACHINE strings,
#     issue #1175): microSD is the only deployment path on the EVK
#     (xSPI-strapped, carrier Ethernet dead per errata E1), and it did
#     not boot a self-built image.  The vendor env hardcodes
#     boot/r9a09g056n44-dev.dtb -- a filename no ALP image builds -- in
#     BOTH sd2load and emmcload, so eMMC (the ALP_BOOT_DEVICE default in
#     every E1M machine conf) was broken the same way and is fixed
#     alongside it.  CONFIG_BOOTCOMMAND now reloads the correct
#     per-MACHINE dtb on either medium, AFTER the leading
#     'env default -a' wipe, inside an 'if ... then ... else echo ... fi'
#     so a miss does NOT fall through to booting a stale devicetree
#     (hush does not abort a ';' list on a failed builtin).  It also
#     fixes the microSD root device (vendor mmcblk2p2 -> mmcblk1p2 per
#     e1m-x-evk.dtsi:88-89).  Both values are injected per-family below
#     (alp-boot-v2n.cfg / alp-boot-v2m.cfg) and both Kconfig
#     defaults are the VENDOR values, so a machine that gets no fragment
#     (stock rzv2n-evk) keeps the vendor VALUES -- but NOT vendor
#     BEHAVIOUR: the guard around the dtb reload is unconditional (it
#     is part of CONFIG_BOOTCOMMAND itself, not build-gated), so a
#     missing boot/r9a09g056n44-dev.dtb now hard-stops the boot instead
#     of falling through silently, on every machine built with this
#     defconfig, ALP fragment or not.  The mmcblk1p2 root is NOT
#     bench-confirmed (see the patch comment).
# VALIDATION: bitbake-built dev + prod with config asserts; the FIP
# (BL2+BL31+u-boot, manual flow) was built 2026-06-12 with both ALP
# patches and the u-boot binary content-verified (alp_root bootcmd +
# pinned console). On-silicon FIP flash + boot is PENDING maintainer
# authorization (a persistent bootloader write to the shared bench
# board).
#
# OPERATIONAL TRAP -- READ BEFORE THE NEXT MANUAL FIP BUILD: the manual
# FIP flow (build_custom_fip_v630_deepx.sh, WSL, outside this repo) has
# no merge_config.sh step, so it builds off the Kconfig DEFAULTS --
# CONFIG_ALP_FDT_FILE="r9a09g056n44-dev.dtb" (the vendor filename) and
# CONFIG_ALP_SD_ROOT="/dev/mmcblk2p2" (the vendor root) -- not the
# per-MACHINE ALP values. Before this change that wrong dtb name fell
# through silently and the board limped along on whatever stale FDT
# was already in RAM; after this change the reload guard is
# unconditional, so the NEXT FIP that script builds hard-stops with
# "refusing to boot a stale devicetree" and does not boot at all.
# REQUIRED ACTION before the next FIP build: the script's .config must
# set CONFIG_ALP_FDT_FILE and CONFIG_ALP_SD_ROOT to the per-MACHINE
# values (see alp-boot-v2n.cfg / alp-boot-v2m.cfg for what to copy in)
# -- this is the difference between a booting bootloader and one that
# reads as bricked. The manual flow was updated the same day to apply
# 0002 alongside the DEEPX patch and to verify it (strings u-boot |
# grep 'setenv alp_root'), but that flow has NOT yet been run with the
# corrected .config -- the boot-dtb fix (CONFIG_ALP_FDT_FILE +
# CONFIG_ALP_SD_ROOT) is UNVERIFIED beyond the hand-checked patch
# application (see the patch's own git-apply/patch --dry-run note).

# CONFIG_ALP_FDT_FILE + CONFIG_ALP_SD_ROOT (issue #1175): the per-MACHINE
# boot dtb filename and microSD root device.  V2N* and V2M* build
# different board dtbs (KERNEL_DEVICETREE differs -- see the four
# conf/machine/e1m-v2{n,m}10{1,2}-a55.conf), so the fragment must be
# picked per SKU.
#
# Selected on MACHINE EXACTLY, deliberately NOT via ":append:<override>".
# MACHINEOVERRIDES is an inherited CHAIN here: e1m-v2m101-a55 declares
# "e1m-v2m101-a55:e1m-v2m101:e1m-v2n101-a55:e1m-v2n101" (V2M is "V2N101 +
# DEEPX"), so a V2M build ALSO carries the V2N tags.  bitbake applies
# EVERY matching :append:<override> -- it does not pick the most specific
# -- so :append:e1m-v2m101-a55 and :append:e1m-v2n101-a55 would BOTH fire,
# both fragments would reach find_cfgs(), CONFIG_ALP_FDT_FILE would be set
# twice and merge_config.sh takes the LAST one merged.  Which one that is
# comes down to bitbake's PARSE order, not to override precedence:
# data_smart.py iterates the :append list in the order the lines were
# parsed and keeps each whose override is in OVERRIDES at all -- it does
# not rank them.  So the winner is simply whichever append line is written
# last in this file, which is a silent, order-dependent coin flip between
# e1m-v2n101-x-evk.dtb and e1m-v2m101-x-evk.dtb for a V2M board.
# (A plain override ASSIGNMENT is a different mechanism but no safer here;
# and e1m-v2n102-a55, whose chain also contains e1m-v2n101-a55, would match
# two append lines and pull the v2n fragment in twice.)
# d.getVar('MACHINE') has no chain and no precedence, so it is the only
# safe selector.  A machine that matches nothing -- stock rzv2n-evk --
# gets no fragment and keeps the vendor-identical Kconfig defaults.
SRC_URI:append:rzv2n-family = "${@' file://alp-boot-v2n.cfg' if d.getVar('MACHINE') in ('e1m-v2n101-a55', 'e1m-v2n102-a55') else ''}"
SRC_URI:append:rzv2n-family = "${@' file://alp-boot-v2m.cfg' if d.getVar('MACHINE') in ('e1m-v2m101-a55', 'e1m-v2m102-a55') else ''}"

# Production boot lockdown (BOOTDELAY=0 + keyed autoboot + the prod
# cmdline above): opt-in for release-bundle builds only. An
# un-overridden prod build has an EMPTY stop string (= no stop
# sequence at all); the internal release pipeline injects the real
# per-product stop string. See prod-boot.cfg for the lock mechanism
# and the deferred saved-env hole.
ALP_PROD_BOOT ?= "0"
SRC_URI:append:rzv2n-family = "${@' file://prod-boot.cfg' if bb.utils.to_boolean(d.getVar('ALP_PROD_BOOT')) else ''}"

# Per-SKU board dtb for CONFIG_BOOTCOMMAND (alp-sdk#1252).  One u-boot
# binary serves both families, so the dtb basename is a Kconfig string
# (CONFIG_ALP_E1M_FDTFILE, patch 0002) whose default suits the V2N SKUs;
# the V2M MACHINEs override it through the same *.cfg channel
# prod-boot.cfg uses (u-boot-configure.inc's find_cfgs() +
# merge_config.sh pick up any *.cfg in SRC_URI).
#
# Scoped by MACHINE, not by rzv2n-family: the family override covers the
# V2N SKUs too, and applying the V2M name there would invert the bug.
# Any new V2M MACHINE needs a line here -- there is no wildcard that is
# safe, because "which dtb does this image contain" is a per-MACHINE fact
# (KERNEL_DEVICETREE), not a family one.
SRC_URI:append:e1m-v2m101-a55 = " file://fdtfile-v2m.cfg"
SRC_URI:append:e1m-v2m102-a55 = " file://fdtfile-v2m.cfg"

# Build U-Boot with the rzv2n-dev config, not the machine's stock rzv2n-evk.
# The DEEPX bring-up patched above lives in board/renesas/rzv2n-dev/rzv2n-dev.c,
# which is ONLY compiled under CONFIG_TARGET_RZV2N_DEV -- so the dev config is
# mandatory, not cosmetic. It also reproduces the silicon-validated manual
# bootloader (DEV target, env-in-MMC, Renesas clock driver off). The longer-term
# rzv2n-dev-vs-rzv2n-evk production-config decision (clock driver, env storage)
# is deliberately deferred -- see the alp-sdk-internal som-productization docs.
UBOOT_CONFIG[rzv2n-evk] = "rzv2n-dev_defconfig"

# Demote patch-fuzz from fatal-error to warning -- ONLY for the
# rzv2n-family u-boot build. A u-boot_%.bbappend fires for EVERY machine's
# u-boot recipe, so the demotion must carry the same :rzv2n-family
# machine override the SRC_URI additions above use; unscoped it would
# relax the fatal patch-fuzz QA gate for every other machine in the
# distro too (issue #245).
# WHY the demotion at all: u-boot here is renesas-u-boot-cip pinned via
# +git (a moving base), and the vendor feature-layer patches
# (meta-rz-drpai add-ether, meta-rz-opencva OpenCVA+Codec) apply on top
# of it. Those vendor hunks land with a small context offset -- they
# apply correctly ("Hunk succeeded"), but Yocto's fatal patch-fuzz QA
# fails do_patch. The offset is structural, not a defect in our patches:
# the OpenCVA hunk in include/configs/rzv2n-evk.h fuzzes by the same
# 5 lines even though NO ALP patch touches evk.h -- that is pure +git
# base drift. (The ALP 0002 patch additionally shifts rzv2n-dev.h,
# compounding it.) Refreshing vendor patches each BSP bump is a
# treadmill and we do not own them, so demote the gate here rather than
# mask fuzz globally.
WARN_QA:append:rzv2n-family = " patch-fuzz"
ERROR_QA:remove:rzv2n-family = "patch-fuzz"
